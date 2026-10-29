// SPDX-License-Identifier: MIT

// Checks the replacement of a sound's sequence (src/edit/replace.h), and of a bank, a wave archive or a wave sound file
// (src/edit/files.h), on a sound archive of the 3DS, the Wii U or the Switch. Like the program, the tool imports the
// archive into a Hokuto file and builds the archive again from the file. Each sequence sound then takes its sequence
// file, the same file for each other console and another sound's file in turn. Each replacement makes the edit that
// Replace… makes:
//
// - The sound's sequence file from the built archive, from the sound's start. The file has to come back unchanged.
// - The same file for each other console among the 3DS, the Wii U and the Switch (song::OtherConsoleSequenceFile). The
//   file has to come back unchanged too. A sequence file counts apart when the bytes that no command reaches come back
//   otherwise from another console. So does a file whose biquad filters come back with other types or values
//   (song::MapBiquads): its other commands have to come back, and Replace… has to offer the sound's start. So does a
//   file that reaches a command that the 3DS lacks: it can't go to the 3DS.
// - The sequence file of the next sequence sound in the sound table that plays another file, from that sound's start.
//   The sound also takes the other sound's bank slots through the edit, in a list of the other sound's length.
//   ntdlib's performer then has to play the other sound's notes and mix in the sound's place, with the tempo, the
//   length and the loop. The performer finds a loop where every track's state repeats. When it finds the loop a whole
//   number of passes away, the notes have to start as before up to the earlier loop's end, and the sound counts
//   apart. The sounds that start at the jump with the sound take the slots as well, and have to play the same. The
//   file's other sounds have to play as before. Every other file of the archive has to keep its bytes, but for the
//   files of groups that hold a copy of the sound's file. A copy in a group file beside the archive has to take the
//   new bytes, and the group files' other copies have to keep theirs. Every other sound has to keep its start.
//
// Each replacement also adds the tracks that the new commands open and the sound's old commands don't
// (edit::TracksToAdd). A file that comes back unchanged exports the archive byte for byte. Replace… offers a file's
// labels and its first command as starts, and the starts of the sounds whose sequence holds the file's commands
// (edit::SoundStarts). The sound's file, and each file for another console that comes back unchanged, have to offer the
// sound's start. A sound whose start is at no label of its file and past its first command counts apart. So does a
// sound whose commands open tracks that it doesn't allocate. The game leaves those tracks out. Such a sound neither
// takes another sound's file nor gives its file to another sound: the tracks would differ.
//
// Each file of a bank, a wave archive or a wave sound then takes three kinds of file in turn, with Replace…'s edit.
// Like the Replace window, the tool reads the file before each edit and finds what it changes:
//
// - The file itself. The Hokuto file has to come back unchanged, but for a file with a copy in a group that holds other
//   bytes. That copy takes the file's bytes, and the file counts apart. A copy of a bank or a wave sound file can name
//   the file's waves at other places, such as in a wave archive of the group's. That copy takes the file with its
//   places, and the Hokuto file comes back unchanged.
// - The file that Export… writes for each other console (song::OtherConsoleFile). It goes in converted, with the
//   versions of the present file (edit::IncomingVersions). The Hokuto file has to come back unchanged too, but for a
//   wave archive whose waves come back with other bytes. Such a wave archive counts apart, and its waves have to decode
//   to the same samples, at the same rate and with the same loop.
// - The next file of the same kind in the archive's list. Every copy of the file has to hold the new bytes. A copy that
//   named the file's waves at other places has to hold the new file with its places instead: each wave that the
//   present file names at the old copy's place for it, and each other wave at its place in the new file. Every other
//   file has to keep its bytes, but for the files of groups that hold a copy. Every sound has to keep its start.
//
// The sounds of a bank can select programs that the next bank lacks (edit::ProgramsOfBank). The next bank then goes in
// again twice with the Replace window's rows of instruments (edit::WithProgramFixes): with the present bank's
// instrument at each such program that the present bank holds, then with the next bank's first instrument at each. The
// copies have to pass the checks of the next file. ntdlib's reader has to read each copy as its instrument, with the
// same regions and the same waves by their wave archives and indices. Every other instrument of the next bank has to
// read as before.
//
// The group files beside the archive count as well. Each copy of the file there has to hold the new bytes, and each
// other copy there its bytes. A file that several items share counts once. A file that two file IDs name counts apart:
// Replace… refuses it. So does a file that neither the archive nor its group files hold. The tool counts the files that
// only the group files hold, and checks them like the others.
//
// Each wave of each wave archive then goes out as a WAV file and comes back in its place. The tool writes the WAV file
// that the browser's Export… writes, and makes Replace…'s edit with it. The WAV file has to hold the wave's rate,
// channels, loop and length. The new wave archive has to hold the new wave in the wave's place and the other waves'
// bytes. The new wave has to decode to the old one's samples. Each wave goes in alone. Then the wave archive with every
// wave from its WAV file goes in as Replace…'s edit of a file, and every wave of the exported archive has to decode to
// its old samples. A WAV file holds a looped wave's first pass. The tool counts apart the looped DSP-ADPCM waves whose
// later passes the console decodes otherwise: those passes start from the loop's context. Each wave also goes back in
// from the wave file that the browser's Export… writes. The wave archive has to hold the same waves. The tool counts
// the waves whose wave archive the writer gives back with other bytes. The wave's file for each other console goes back
// in too, converted with the wave's version. The new wave has to decode to the old one's samples.
//
// Each wave also comes back from its WAV file as DSP-ADPCM, the Replace window's first choice. A loop's start moves
// forward to a frame of 14 samples, and the samples from the old start repeat after the loop's end. The new wave has to
// decode close to those samples, with the ratio of signal to noise that the Replace window shows. Each later pass of
// its loop has to decode to the first pass's samples. The new wave archive has to keep the other waves' bytes. The tool
// counts the waves by their ratio of signal to noise.
//
// Each wave above 8,000 Hz also comes back from its WAV file at half its rate. The Replace window's Sample rate menu
// offers that rate. The new wave has to hold the samples that Hokuto's resampler gives each channel at the new rate, as
// 16-bit PCM. Its loop has to start at the old start's place at the new rate. A looped wave's WAV file also goes in
// without its loop and takes the present wave's loop. The Replace window offers that choice. The new wave has to hold
// the bytes that the WAV file with the loop gives.
//
// Where the windows propose groups for a new or replaced item, the item joins them through the windows' edit
// (src/edit/groups.h). Up to 8 items of each archive join groups in each of the checks of the sequences, the files of
// banks, wave archives and wave sounds, --new and --wave-sounds. The first join of each check also takes a group that
// the windows don't propose: one beside the archive where the Hokuto file holds one. A group that would take a file
// that Hokuto can't read is unavailable, and the tool counts it. The edit refuses such a group that the windows don't
// propose, and a group that links to the archive's files and would take a file that only groups hold. The window shows
// the refusal, and the tool counts it. Each chosen group has to hold each file that the items require, but for the
// files of the old item or the model where it loads them. It has to take no other file. It has to take copies where it
// held copies. A group without files has to take copies when a link can't reach one of the files, and links otherwise.
// A link can't reach a file that the FILE block doesn't hold at a place of the file table. The group has to keep its
// files and items first, and to list each item whose file it didn't hold and that it didn't list, once. The load flags
// have to name every part exactly when the group holds every file that the item requires. The other groups and group
// files, the other files' bytes and the items' tables have to stay. Each file's list of groups has to gain the groups
// that took a copy of the file or a link to it, but for a link to a wave archive's file. The 3DS's file table holds no
// lists. The file has to save and open again in both forms, and the same join again has to change nothing.
//
// The first item of each of those checks also joins a new group, in every archive, through the windows' edit
// (edit::AddGroups). The archive with the new group has to hold the other groups, files, group files and names as
// before. The new group has to follow them, with an empty group's file at the end of the file table. The game's sizes
// have to give it a size of 0, and the file has to save and open again in both forms. The new group then has to take
// the items as a group without files does. On the 3DS, its file has to take the highest version of the groups' files
// inside the archive. The edit refuses a new group that would take a file that Hokuto can't read, and a new group in an
// archive without names. The window shows the refusal, and the tool counts it.
//
// With --new, the tool checks only the new sounds of File > Import > Sequence… (src/edit/new_items.h). Up to 32
// sequence sounds, spread through the sound table, each serve as a model. Each model's new sound takes the model's
// sequence file, from the model's start, through the edit that the Import window makes. The new sound has to hold the
// model's values and play the model's notes. A model whose commands open tracks that it doesn't allocate counts apart:
// its new sound allocates those tracks. Every other item has to keep its values, and every file its bytes. The program
// has to find the new sound's sequence, and the file has to save and open again in both forms. The model's file for
// each other console has to give the same archive. A file that decodes otherwise from another console counts apart. So
// does a file whose biquad filters come back with other types or values, and a file that reaches a command that the 3DS
// lacks. The second model's new sound also goes in after the first model's.
//
// With --banks, the tool checks only File > Import > Bank… and Wave Archive…. Up to 8 sequence sounds, spread through
// the sound table, each serve as a model. Each bank in the model's slots goes in again as a new bank, through the edits
// that the Import windows make. Each wave archive that the bank's waves name goes in first, as a new wave archive with
// the old one's file. The new bank's waves then lie in the new wave archives. The model's sequence file then goes in as
// a new sound, with the new banks in the model's slots. The new sound has to play the model's samples for 10 s on
// ntdlib's model of the console. Every other item has to keep its values, and every file its bytes. The new wave
// archives' files have to hold the old ones' bytes. The new banks' files have to hold the old ones' bytes but for the
// wave archives that their waves name. A model whose banks or wave archives lack their files counts apart. So does a
// model that starts where no command of its file starts, or whose commands open tracks that it doesn't allocate.
// Another model then takes its place.
//
// With --wave-sounds, the tool checks only File > Import > Wave Sound File…. Up to 8 wave sound files of a donor go in
// as new wave sounds, through the edits that the Import windows make. The donor is the archive itself unless a second
// archive follows it. Each wave archive that the file's waves name goes in first, as a new wave archive with the old
// one's file. The new file's waves then lie in the new wave archives. The tool reads each file as the Import window
// reads it (edit::ReadIncomingFile): a file of a later version than the archive's files goes in with their versions.
// When the donor is the archive itself, the new sounds copy the first sound that plays the file. Otherwise they copy
// the archive's first wave sound, if it has one. The file also goes in with the archive's first sequence sound as the
// model. Each new sound has to hold the model's values and play the samples that the donor's sound at its index plays
// (song::DecodeWaveSound). Every other item has to keep its values, and every file its bytes. The new wave archives'
// files have to hold the bytes that the read gives, and the new file those bytes but for the wave archives that its
// waves name. The file has to save and open again in both forms. A file whose wave archives lack their files counts
// apart.
//
// With --new-waves, the tool checks only File > Import > Wave… and Audio File…. Each wave of each wave archive goes in
// again as a new wave after the wave archive's waves: from its wave file, from its wave file for each other console and
// from its WAV file as 16-bit PCM, through the reads of the Import window. The first also goes in from its WAV file as
// DSP-ADPCM. A wave archive without waves takes the archive's first wave, or a short WAV file in an archive without
// waves. The new wave archive has to hold the old waves' bytes and the new wave after them. The new wave has to decode
// to the samples of the wave that it came from. A wave that Hokuto makes or converts has to take the version of the
// wave archive's first wave. So does a wave file of a later version. Hokuto writes it again. The first wave's file then
// goes in through the Import window's edit. The exported archive has to hold the new wave archive in every copy, and
// every other file has to keep its bytes. In one wave archive of each archive, a second new wave goes in after the
// first, and the file has to save and open again in both forms.
//
// With --streams, the tool checks only the streams beside the archive (src/edit/streams.h), each with the first stream
// sound that plays it. Replace… with the stream itself has to give the file back unchanged. The stream's file for the
// other console goes back in as the browser's Export… writes it: the Wii U's file for a stream of the Switch or the
// 3DS, and the Switch's for one of the Wii U. It takes the version that the archive's streams take
// (edit::IncomingStreamVersion). A stream of that version has to come back with the same bytes, and any other has to
// play the same samples in that version. A 3DS stream has to play the same samples: the Wii U's file drops the samples
// past its end, and its original loop. On the Wii U, a stream of a later version than 3.0.0 also goes in written again
// in 3.0.0, and on the 3DS one of a later version than 2.0.0.0 in 2.0.0.0. It has to play the same samples, and then go
// in as it is. The stream's WAV file from Export… goes back in as 16-bit PCM. The new stream has to play the same
// samples: a loop's start moves forward to a block, and the samples from the old start repeat after the loop's end. The
// export then has to give the archive back unchanged, and the file has to hold the new stream as Hokuto's. The WAV file
// also goes in as DSP-ADPCM, and the tool counts the streams by their ratio of signal to noise. Then File > Import >
// Stream… brings the stream in again as a new stream sound with the sound as its model (src/edit/new_items.h). The new
// sound has to hold the model's values and the new details, and play the stream from a new file at the new path. The
// file has to hold the new stream as Hokuto's, and every other item, file and stream has to stay. In each archive, a
// second new sound goes in after the first, from the first stream's WAV file and with a sound of another kind as its
// model where the archive holds one. The file has to save and open again in both forms.
//
// With --streams and a donor archive after the archive, each stream beside the donor goes in as a new stream sound of
// the archive instead, from the stream's file. The donor's streams thus reach archives without streams beside them. The
// model alternates between the archive's first stream sound and its first sound of another kind. The new stream has to
// play the donor's samples, and the new sound has to hold its model's values and the new details, as above. Where the
// sound plays the tracks of its stream file, a stream without tracks has to take a track for each pair of its channels.
// An AAC stream of the 3DS beside a donor counts apart: File > Import > Stream… refuses it. An AAC stream beside the
// archive takes Replace…'s edit of an AAC stream instead (edit::ReplaceAacStream). So does the first stream sound whose
// entry marks an AAC stream that the file lacks, with the WAV file of the first AAC stream that the file holds. The new
// stream has to be AAC, with the channels that the sound's entry gives and the WAV file's rate. A present AAC stream's
// file goes in as it is, with the present loop, and the file in the model form has to hold the game's model. Its WAV
// file and its file for the Wii U go in encoded by the system's encoder. Export… writes both. The new stream has to
// decode to their samples after the encoder's delay, at a signal-to-noise ratio of 15 dB or more, and 3 dB above the
// ratios a sample before and after the delay. The entries of the sounds that play it have to give the loop after the
// delay, and every other value of the model has to stay. After the first new sound, the file has to save and open again
// in both forms. An archive without names, and a 3DS archive of CSAR 2.1.0.0 or 2.3.1.0, take no new stream sound. The
// edit has to refuse it, and the tool counts the stream apart.
//
// With --variables, the tool checks only the variables of Replace… of a sequence (src/edit/variables.h). Each sequence
// sound whose commands read variables before they set them takes its sequence file again, from its start, in three
// rounds. In the first, each such variable moves to the lowest number of its kind that the commands don't use. The
// sound then has to play each value of each variable at the new number as it played the value at the old number. A
// variable's values are its choices, or else 0 and 1. In the second round, each variable takes its second value as a
// fixed value, and in the third its last. Like Replace…, the second leaves out the variables that the sound's commands
// read now: the game's code sets those. The sound then has to play without starting values, and with others, as it
// played with the fixed values. Up to 4 sounds of each sequence file take the rounds, one in each version of the file.
//
// With --sequences, the tool checks only the sequences. With --files, it checks only the files of banks, wave archives
// and wave sounds. With --waves, it checks only the waves. It prints a line for each sound, file or wave archive that
// fails, and a summary. It returns 1 when one fails, and 2 when the archive can't be read.
//
// A 3DS archive's sounds play on ntdlib's model of the DSP's audio firmware.
//
// An archive of the Wii (RSAR) imports with the game's build of NW4R SND, from the main.dol of the extracted disc that
// holds the archive, or else with the build that the archive's version gives. Each sequence sound takes its sequence
// file again from its start, and the Hokuto file has to come back unchanged. It then takes the file of the next
// sequence sound that plays another file, with that sound's bank: a sound of the Wii plays one bank. ntdlib's performer
// has to play the other sound's notes in the sound's place, by the rules of the build. The file's other sounds have to
// play as before. Every copy of the sound's file in the archive's groups has to hold the new commands, and every other
// file has to keep its bytes and its wave data in every copy. Each bank's file, each wave sound file and each RWAR of a
// file's wave data then takes its bytes again, and the Hokuto file has to come back unchanged. It then takes the next
// file of its kind that keeps its waves in the same way: in a WAVE block, or in an RWAR. Every copy has to hold the new
// bytes, with zeros up to a multiple of 32 bytes. The file's other part, every other file and every sound's start have
// to stay. Each wave of each file's wave data then goes back in its place from its wave file (RWAV), and the Hokuto
// file has to come back unchanged. The wave's WAV file from Export… then goes in as 16-bit PCM and as DSP-ADPCM. The
// new wave has to hold the wave's rate, channels and loop, and the other waves of the wave data their values. In 16-bit
// PCM it has to decode to the wave's samples. In DSP-ADPCM its loop may move forward to the next frame, and the tool
// counts the waves by their ratio of signal to noise. A wave of more than two channels counts apart: the WAV file holds
// two. So does a wave whose loop holds no samples: a WAV file can't hold such a loop, and the wave comes back without
// it. The tool counts Replace…'s refusals by their reasons.
//
// With --streams, the tool checks only the streams beside an archive of the Wii (src/edit/streams.h), each with the
// first stream sound that plays it. Replace… with the stream file itself has to give the Hokuto file back unchanged.
// The stream's files that the browser's Export… writes for the 3DS, the Wii U and the Switch then go back in converted.
// Each has to play the stream's samples and keep its tracks. The tool counts those that give the stream's bytes. The
// stream's WAV file goes in as 16-bit PCM, and the new stream has to play the same samples with the present stream's
// tracks. A loop's start moves forward to a block there. The export then has to differ from the archive only in the
// stream's size in the file table. Replace… with the stream file then has to give the archive back. The WAV file also
// goes in as DSP-ADPCM. Its loop has to start at a block, at the present start or after it, and each later pass has to
// decode to the first pass's samples with the Wii's sums. The tool counts the streams by their ratio of signal to
// noise. Then File > Import > Stream… brings the stream in again as a new stream sound with the sound as its model
// (edit::AddWiiStreamSound). The new sound has to hold the model's values, and its entry has to name every channel and
// track of the stream. The new file has to lie beside the archive at the new path, with the stream's size, and in no
// group. The Hokuto file has to keep the new stream as Hokuto's, and every other item, file, group and stream has to
// stay. The new sound has to play as the Import window plays it. Where the model's entry names every channel and track
// of the stream, the new sound also has to play as the model does. In each archive, a second new sound goes in after
// the first, from the first stream's WAV file, with a sound of another kind as its model. The file has to export the
// same archive in the model form, and save and open again in both forms. An archive that NW4R's tools didn't lay out
// takes no new sound, and the edit has to refuse it. With a donor archive of any console after the archive, each stream
// beside the donor goes in as a new stream sound instead, as above. The model alternates between the archive's first
// stream sound and its first sound of another kind, each at a volume above 0. A stream of a later console without
// tracks has to take a track for each pair of its channels.
//
// --new, --banks, --wave-sounds and --new-waves check the Wii's imports through the archive's model
// (edit::AddWiiSequenceSound, AddWiiBank, AddWiiWaveSounds and WithIncomingWiiWave). Up to 32 sequence sounds serve as
// models with --new. Each new sound plays its model's file from the model's start, and joins the groups that hold the
// model's file. It has to hold the model's values and play the model's notes. With --banks, the bank of each of up to 8
// models goes in again with a copy of its wave data, and a copy of the model plays the new bank. It has to play the
// model's samples for 10 s on ntdlib's models of NW4R SND and AX. With --wave-sounds, the files of up to 8 wave sounds
// go in again with their wave data, once with the wave sound as the model and once with the first sequence sound. The
// new sound in the wave sound's place has to play the wave sound's wave. With --new-waves, the first wave of each wave
// data goes in again after the waves, from its wave file and from its WAV file. After each import, every other item has
// to keep its values, every other file its bytes and every group its files, and the file's sequences their numbers. An
// archive that NW4R's tools didn't lay out takes no new item. Only these eight options take an archive of the Wii.
//
// With a donor after an archive of the Wii, --new, --wave-sounds, --new-waves and --files check the files of the donor,
// an archive of the 3DS, the Wii U or the Switch, in the Wii's archive. With --new, up to 8 of the donor's sequence
// sounds come in with the bank of their first slot and the wave archives that the bank names. Each wave archive comes
// in as a bank without instruments that holds its waves. File > Import > Wave Archive… brings one in that way on the
// Wii. Its waves have to keep the donor's rates, loops and the samples of their first two channels. The bank comes in
// with the waves of those banks' wave data in place of its wave archives (edit::WiiFileOf). The sound comes in
// converted, from the label of its file where it starts. It copies the values of the archive's first sequence sound,
// and then takes the volume, the pan mode and the pan curve of Another Console's conversion of the donor's sound
// (edit::ForWii). It then has to play the conversion's samples for 10 s by the rules of the archive's build. File >
// Import brings in the bank of one slot, whole. A conversion can take the banks of several slots as one bank, or a bank
// with only the instruments that the sound selects. The tool counts such a sound apart, and the next of the donor's
// sequence sounds takes its place. An archive before version 1.2 holds no pan mode and no pan curve. NW4R SND pans the
// sounds of such an archive in balance mode, with the square-root curve. The conversion takes those values for the
// reference, and the tool counts such conversions. With --wave-sounds, up to 8 of the donor's wave sound files come in
// with their wave archives in the same way. Each new wave sound has to play the wave of the donor's wave sound at the
// same rate. With --new-waves, the first wave of up to 8 of the donor's wave archives goes in after the waves of the
// archive's first wave data, and in the place of its first wave. With --files, each of up to 8 banks and 8 wave sound
// files takes the donor's first file of its kind in its place. The wave archives that the donor's file names map to the
// present file's wave data: the Replace window's first proposal. Every copy of the file and of its wave data has to
// take the new bytes, and every other file has to keep its bytes.
//
// With --model, the file goes into the model form after the import (format::InModelForm), and the archive that the
// model builds has to be the game's. A donor stays in the native form. Of the files that the edits make, the first 64
// and every 64th after them have to save in both forms and open again as they were (tools/checked_archive.h).
//
// usage: hokuto_check_replace [--model] [--sequences | --files | --waves | --new | --banks | --new-waves |
//                                        --streams | --variables] <archive>
//        hokuto_check_replace [--model] --wave-sounds <archive> [<donor>]
//        hokuto_check_replace [--model] --streams <archive> [<donor>]
//        hokuto_check_replace [--model] --new | --new-waves | --files <Wii archive> <donor>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ntdlib/rsar/archive_model.h"
#include "ntdlib/rsar/bank_file.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/rsar/sequence_file.h"
#include "ntdlib/rsar/stream_file.h"
#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/decode.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/groups.h"
#include "ntdlib/sar/info_block.h"
#include "ntdlib/sar/sequence_file.h"
#include "ntdlib/sar/stream_file.h"
#include "ntdlib/sar/string_block.h"
#include "ntdlib/sar/tables.h"
#include "ntdlib/sar/wave_file.h"
#include "ntdlib/sequence_export/performer.h"
#include "ntdlib/wave/adpcm.h"
#include "ntdlib/wave/decode.h"
#include "seqlib/wav.h"
#include "xsflib/binary.h"
#include "xsflib/console.h"
#include "xsflib/files.h"

#include "audio/resampler.h"
#include "checked_archive.h"
#include "edit/banks.h"
#include "edit/bytecode.h"
#include "edit/files.h"
#include "edit/groups.h"
#include "edit/new_items.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "edit/tables.h"
#include "edit/variables.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/biquads.h"
#include "song/dry_render.h"
#include "song/items.h"
#include "song/preview.h"
#include "song/sequence.h"
#include "song/sequence_model.h"
#include "song/streams.h"

namespace
{

namespace audio = hokuto::audio;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;

using Archive = std::shared_ptr<const sar::SoundArchive>;

// What the check counted.
struct Totals
{
    std::size_t sounds = 0;
    std::size_t failed = 0;
    std::size_t in_bytes = 0;
    std::size_t unlabelled = 0;
    std::size_t other_unreached = 0;
    std::size_t other_biquads = 0; // files for another console whose biquad filters come back otherwise
    std::size_t ctr_refused = 0;   // files that reach a command that the 3DS lacks
    std::size_t crossed = 0;
    std::size_t untracked = 0;
    std::size_t loops_moved = 0; // replacements whose loop the performer found a whole number of passes away
    std::size_t files = 0;
    std::size_t files_failed = 0;
    std::size_t files_crossed = 0;
    std::size_t files_shared = 0;
    std::size_t files_outside = 0; // in neither the archive nor its group files
    std::size_t files_beside = 0;  // in the group files beside the archive alone
    std::size_t files_other_copies = 0;
    std::size_t files_placed = 0;              // files with copies in groups that name their waves at other places
    std::size_t files_placed_copies = 0;       // such copies that took another file with their places
    std::size_t files_converted = 0;           // files that took the same file from another console
    std::size_t files_converted_otherwise = 0; // wave archives whose waves came back from it with other bytes
    std::size_t files_written = 0; // other files of the kind that went in written again with the archive's versions
    std::size_t wave_archives = 0;
    std::size_t wave_archives_failed = 0;
    std::size_t wave_archives_out = 0;    // in neither the archive nor its group files, or shared by two file IDs
    std::size_t wave_archives_beside = 0; // in the group files beside the archive alone
    std::size_t waves = 0;
    std::size_t waves_wide = 0;        // waves of more than two channels
    std::size_t waves_loop_lost = 0;   // waves whose loop flag the WAV file doesn't keep
    std::size_t waves_later = 0;       // looped DSP-ADPCM waves whose later passes differ from the first
    std::size_t waves_rewritten = 0;   // waves whose files give their wave archives other bytes in their places
    std::size_t waves_other_bytes = 0; // waves that come back with other bytes from another console
    int later_peak = 0;                // the largest difference of a sample there
    double later_ms = 0.0;             // the longest time from the loop's start to the last difference

    // The waves from their WAV files as DSP-ADPCM.
    std::size_t adpcm_exact = 0;      // waves that come back exactly
    std::size_t adpcm_moved = 0;      // waves whose loop's start moves forward to a frame
    std::vector<double> adpcm_ratios; // the signal-to-noise ratios of the others, in decibels
    std::size_t adpcm_samples = 0;    // the samples that the encoder took, in all channels
    double adpcm_seconds = 0.0;       // the time that the reading of the WAV files took

    // The waves from their WAV files at half their rates, and without their loops.
    std::size_t resampled = 0;         // waves that came back at half their rates
    std::size_t resampled_low = 0;     // waves that keep their rates of 8,000 Hz or less
    std::size_t resampled_samples = 0; // the new samples, in all channels
    double resampled_seconds = 0.0;    // the time that the reading of the WAV files took
    std::size_t loops_offered = 0;     // looped waves that took the present wave's loop

    // The programs that the sounds of a bank select and the next bank lacks.
    std::size_t fixed_banks = 0;    // banks whose next bank lacks such programs
    std::size_t fixed_programs = 0; // the programs
    std::size_t fixes_present = 0;  // programs that took the present bank's instrument
    std::size_t fixes_next = 0;     // programs that took the next bank's first instrument
    std::size_t selected = 0;       // the programs that the sounds of the banks select
    std::size_t undecided = 0;      // program changes of those sounds whose program or slot a value decides

    // The new sounds of --new.
    std::size_t new_sounds = 0;
    std::size_t new_failed = 0;
    std::size_t new_crossed = 0;         // the same new sounds from the files for the other consoles
    std::size_t new_pairs = 0;           // new sounds after another new sound
    std::size_t new_no_start = 0;        // models that start where no command of their files starts
    std::size_t new_untracked = 0;       // models whose commands open tracks that they don't allocate
    std::size_t new_other_unreached = 0; // files for another console that decode otherwise
    std::size_t new_other_biquads = 0;   // files for another console whose biquad filters come back otherwise
    std::size_t new_ctr_refused = 0;     // files that reach a command that the 3DS lacks
    double new_slowest = 0.0;            // the longest time that the edit took, in seconds

    // The new banks and wave archives of --banks.
    std::size_t bank_checked = 0;
    std::size_t bank_failed = 0;
    std::size_t new_banks = 0;
    std::size_t new_wave_archives = 0;
    std::size_t bank_silent = 0;  // new sounds whose 10 s hold no sample above 0
    std::size_t bankless = 0;     // models without a bank in their slots
    std::size_t bank_missing = 0; // models whose banks or wave archives lack their files
    std::size_t bank_skipped = 0; // models that start where no command starts, or that leave out tracks
    double bank_slowest = 0.0;    // the longest time that an edit took, in seconds

    // The new wave sounds of --wave-sounds.
    std::size_t wsd_files = 0; // wave sound files that went in as new wave sounds, once for each model
    std::size_t wsd_failed = 0;
    std::size_t wsd_sounds = 0;         // new wave sounds
    std::size_t wsd_from_sequences = 0; // new wave sounds that copied a sequence sound
    std::size_t wsd_compared = 0;       // new wave sounds that played the donor's samples
    std::size_t wsd_undecoded = 0;      // new wave sounds whose samples neither side decodes
    std::size_t wsd_unplayed = 0;       // new wave sounds of wave sounds that no sound of the donor plays
    std::size_t wsd_missing = 0;        // files whose wave archives lack their files
    std::size_t wsd_wave_archives = 0;  // new wave archives
    std::size_t wsd_written = 0;        // wave archives and wave sound files written again with the archive's versions
    double wsd_slowest = 0.0;           // the longest time that an edit took, in seconds

    // The new waves of --new-waves.
    std::size_t nw_wave_archives = 0; // wave archives whose waves went in again as new waves
    std::size_t nw_failed = 0;
    std::size_t nw_waves = 0;          // waves that went in from each of their files
    std::size_t nw_empty = 0;          // wave archives without waves: they took the archive's first wave
    std::size_t nw_waveless = 0;       // wave archives without waves in archives without waves
    std::size_t nw_other_bytes = 0;    // waves whose file for another console came back with other bytes
    std::size_t nw_other_versions = 0; // waves of another version than a new wave of their wave archive takes
    std::size_t nw_written = 0;        // waves of a later version that went in written again with that version
    std::size_t nw_pairs = 0;          // second new waves after a first
    double nw_slowest = 0.0;           // the longest time that an edit took, in seconds

    // The streams of --streams.
    std::size_t st_streams = 0; // streams beside the archive that a sound plays
    std::size_t st_failed = 0;
    std::size_t st_channels = 0;
    std::size_t st_unplayed = 0;       // streams beside the archive that no sound plays
    std::size_t st_aac = 0;            // AAC streams of the 3DS beside a donor: File > Import > Stream… refuses them
    std::size_t st_aac_replaced = 0;   // AAC streams beside the archive that took their files and were encoded again
    std::size_t st_aac_missing = 0;    // AAC streams that the file lacks: each took another stream's WAV file
    std::vector<double> st_aac_ratios; // the signal-to-noise ratios of the encoded AAC streams, in decibels
    uint32_t st_aac_delay = 0;         // the encoder's delay, in samples
    std::size_t st_moved = 0;          // streams whose loop's start moves forward to a block of 16-bit PCM
    std::size_t st_written = 0;        // streams of the Wii U or the 3DS that went in written again in an older version
    std::size_t st_tracked = 0;        // streams whose sounds play the tracks of their stream files
    std::size_t st_same_details = 0;   // new sounds whose details hold the model's
    std::size_t st_exact = 0;          // streams that come back exactly as DSP-ADPCM
    std::vector<double> st_ratios;     // the signal-to-noise ratios of the others, in decibels
    std::size_t st_pairs = 0;          // second new sounds after a first
    std::size_t st_nameless = 0;       // streams of an archive without names: such an archive takes no new sound
    std::size_t st_unknown = 0;        // donors' streams for a 3DS archive whose layout of stream sounds is unknown
    std::size_t st_paired = 0;         // donors' streams that took a track for each pair of their channels
    double st_slowest = 0.0;           // the longest time that a new sound's edit took, in seconds

    // The variables of --variables.
    std::size_t va_reading = 0;  // sequence sounds whose commands read variables before they set them
    std::size_t va_checked = 0;  // such sounds that the rounds edited
    std::size_t va_failed = 0;   // edits after which a sound plays otherwise
    std::size_t va_refused = 0;  // edits that the variables' map or the replacement refused
    std::size_t va_moved = 0;    // variables that a move gave another number
    std::size_t va_fixed = 0;    // variables that took a fixed value
    std::size_t va_held = 0;     // fixed variables that another of the sound's variables holds
    std::size_t va_runs = 0;     // reads of an edited sound with starting values, compared with a read before the edit
    std::size_t va_no_start = 0; // sounds that start where no command of their files starts
    std::size_t va_runaway = 0;  // fixed values that left a track running without waits, and played otherwise
    std::set<std::string> va_reasons; // why the edits were refused

    // The groups that new and replaced items join.
    std::size_t join_items = 0; // items, or sets of new items, that joined groups
    std::size_t join_failed = 0;
    std::size_t join_refused = 0;    // joins that the edit refuses, with a message that the window shows
    std::size_t join_unproposed = 0; // items for which the windows propose no group
    std::size_t join_unreadable = 0; // groups that load the item and would take a file that Hokuto can't read
    std::size_t join_groups = 0;     // groups that took items
    std::size_t join_loading = 0;    // groups that loaded the old item or the model
    std::size_t join_beside = 0;     // groups beside the archive
    std::size_t join_copies = 0;     // files that the groups took as copies
    std::size_t join_links = 0;      // files that the groups took as links
    std::size_t join_listed = 0;     // items that the groups listed
    std::size_t join_partial = 0;    // items that the groups listed with some of their parts
    std::size_t join_unlisting = 0;  // groups whose files hold no INFX block, and list no items
    double join_slowest = 0.0;       // the longest time that a join took, in seconds

    // The new groups that the first item of each check joins.
    std::size_t made_groups = 0; // new groups that took items
    std::size_t made_failed = 0;
    std::size_t made_refused = 0;   // new groups that the edit refuses, with a message that the window shows
    std::size_t made_copies = 0;    // files that the new groups took as copies
    std::size_t made_links = 0;     // files that the new groups took as links
    std::size_t made_listed = 0;    // items that the new groups listed
    std::size_t made_unlisting = 0; // new groups whose files hold no INFX block, and list no items
};

// A group of an archive: its file, and the files that it loads.
struct Group
{
    uint32_t file_id = 0;
    std::vector<uint32_t> loads;
};

// The groups of the archive in `bytes`. The program reads a Wii U archive past damage, and ntdlib's reader then lists
// no groups. The groups therefore come from a strict read.
std::vector<Group> GroupsOf(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    sar::Options options;
    options.flavor = flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    const sar::SoundArchive strict = sar::SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end()), options);
    std::vector<Group> groups;
    for (std::size_t group = 0; group < strict.Groups().size(); group++)
    {
        groups.push_back({strict.Groups()[group].file_id, strict.GroupFileIds(group)});
    }

    return groups;
}

// The type of reference from a file's entry to its place in the FILE block.
constexpr uint16_t kInternalFile = 0x220C;

// The most items of an archive that join groups in each check.
constexpr std::size_t kJoins = 8;

// ntdlib's reading of the archive that a Hokuto file builds, with the group files beside it, when the reader stops at
// the first problem. Each group's contents follow: nothing for a group whose file the Hokuto file lacks or ntdlib can't
// read. Then whether each group's file holds an INFX block: the list of the group's items.
struct StrictRead
{
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> group_files;
    std::optional<sar::SoundArchive> archive;
    std::vector<std::optional<sar::GroupContents>> groups;
    std::vector<bool> listing;
};

// Whether `group`, a group's file of `flavor`, holds an INFX block: its header names a third block. The 3DS's version
// 1.0.0.0 holds none.
bool Listing(std::span<const uint8_t> group, sar::Flavor flavor)
{
    return xsflib::Reader(group, flavor == sar::Flavor::kCafe).U16(0x10) > 2;
}

// The archive that `file` builds, read strictly with its group files (StrictRead).
StrictRead ReadStrictly(const format::File& file)
{
    StrictRead read;
    read.bytes = format::ExportArchive(file);
    read.group_files = format::ExportGroupFiles(file);
    sar::Options options;
    options.flavor = file.native->flavor;
    options.rules = sar::Rules::kStopAtFirstProblem;
    read.archive = sar::SoundArchive::Load(read.bytes, options);
    for (const std::vector<uint8_t>& group_file : read.group_files)
    {
        read.archive->AddGroupFile(group_file);
    }
    for (uint32_t g = 0; g < read.archive->Groups().size(); g++)
    {
        std::optional<sar::GroupContents>& contents = read.groups.emplace_back();
        const uint32_t file_id = read.archive->Groups()[g].file_id;
        std::span<const uint8_t> group;
        if (file_id != sar::kExternalGroupFile)
        {
            group = read.archive->FileData(file_id);
        }
        for (std::size_t k = 0; k < read.group_files.size() && file_id == sar::kExternalGroupFile; k++)
        {
            if (file.native->group_files[k].group == g)
            {
                group = read.group_files[k];
            }
        }
        try
        {
            contents = sar::ReadGroupFile(group, options.flavor);
        }
        catch (const std::exception&)
        {
            contents.reset();
        }
        read.listing.push_back(contents && Listing(group, options.flavor));
    }

    return read;
}

// The list of groups in each file's entry of the archive in `bytes`, an archive of `flavor`, by file ID. The entry of a
// file that the FILE block doesn't hold has no list. The 3DS's file table holds no lists, and gives none.
std::vector<std::optional<std::vector<uint32_t>>> ListsOfGroups(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    std::vector<std::optional<std::vector<uint32_t>>> lists;
    if (flavor == sar::Flavor::kCtr)
    {
        return lists;
    }
    const sar::InfoBlock info = sar::InfoBlock::Read(sar::TablesOf(bytes, flavor).info, flavor);
    for (const std::vector<uint8_t>& entry : info.files)
    {
        std::optional<std::vector<uint32_t>>& list = lists.emplace_back();
        const xsflib::Reader r(entry, flavor == sar::Flavor::kCafe);
        if (r.U16(0) != kInternalFile)
        {
            continue;
        }

        // The list follows the file's place: a reference to the bytes, their size and a reference to the list.
        const std::size_t location = r.U32(4);
        const std::size_t at = location + r.U32(location + 16);
        list.emplace();
        for (uint32_t k = 0; k < r.U32(at); k++)
        {
            list->push_back(r.U32(at + 4 + std::size_t{k} * 4));
        }
    }

    return lists;
}

// An item ID in hexadecimal.
std::string HexOf(uint32_t id)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", id);

    return text;
}

// What is wrong with `now`, a group's copy of a file after `bytes` took the place of `present`, the file that the
// archive's file table names. The copy held `old`. A copy that held `present`'s bytes, or other bytes besides those of
// its table of waves, has to hold `bytes`. A copy of a bank file or a wave sound file that named `present`'s waves at
// other places has to hold `bytes` with the old copy's places: each wave of `bytes` that `present` names at the place
// where the old copy named it, and each other wave at its place in `bytes`. Such a copy counts in `placed`. A wave of
// `present` with two places in the old copy makes the copy hold `bytes`.
std::string CopyProblem(std::span<const uint8_t> now, std::span<const uint8_t> old, std::span<const uint8_t> present,
                        std::span<const uint8_t> bytes, sar::Flavor flavor, std::size_t& placed)
{
    std::vector<sar::WaveId> was;
    std::vector<sar::WaveId> old_places;
    bool table = false;
    try
    {
        was = sar::WaveTable(present, flavor);
        old_places = sar::WaveTable(old, flavor);
        table = !std::ranges::equal(old, present) && was.size() == old_places.size() &&
                std::ranges::equal(sar::WithWaveTable(present, flavor, old_places), old);
    }
    catch (const std::exception&)
    {
        // A wave archive, a sequence file, or a file whose table ntdlib can't read.
    }
    for (std::size_t i = 0; table && i < was.size(); i++)
    {
        for (std::size_t j = 0; j < i; j++)
        {
            table = table && (was[j] != was[i] || old_places[j] == old_places[i]);
        }
    }
    if (!table)
    {
        return std::ranges::equal(now, bytes) ? "" : "doesn't hold the new bytes";
    }

    placed++;
    const std::vector<sar::WaveId> waves = sar::WaveTable(bytes, flavor);
    const std::vector<sar::WaveId> places = sar::WaveTable(now, flavor);
    if (places.size() != waves.size() || !std::ranges::equal(sar::WithWaveTable(now, flavor, waves), bytes))
    {
        return "doesn't hold the new bytes besides its table of waves";
    }
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        const auto named = std::ranges::find(was, waves[i]);
        const sar::WaveId expected =
            named != was.end() ? old_places[static_cast<std::size_t>(named - was.begin())] : waves[i];
        if (places[i] != expected)
        {
            return "names wave " + std::to_string(i) + " at another place than the old copy";
        }
    }

    return {};
}

// What is wrong with group file `now` of `flavor` after file `file_id` took `bytes` in the place of `present`, or
// nothing. The group file was `was` before. Each copy of the file has to hold what CopyProblem requires, and each other
// copy its bytes. The group file has to keep its files and items. `name` names the group file in the message.
std::string GroupFileProblem(std::span<const uint8_t> was, std::span<const uint8_t> now, sar::Flavor flavor,
                             uint32_t file_id, std::span<const uint8_t> present, std::span<const uint8_t> bytes,
                             const std::string& name, std::size_t& placed)
{
    const sar::GroupContents after = sar::ReadGroupFile(now, flavor);
    const sar::GroupContents before = sar::ReadGroupFile(was, flavor);
    if (after.items != before.items || after.files.size() != before.files.size())
    {
        return "changes the lists of " + name;
    }
    for (std::size_t m = 0; m < after.files.size(); m++)
    {
        const sar::GroupMember& member = before.files[m];
        if (after.files[m].file != member.file || after.files[m].copy.has_value() != member.copy.has_value())
        {
            return "changes the copy of file " + std::to_string(member.file) + " in " + name;
        }
        if (member.copy && member.file == file_id)
        {
            const std::string problem = CopyProblem(*after.files[m].copy, *member.copy, present, bytes, flavor, placed);
            if (!problem.empty())
            {
                return "has a copy in " + name + " that " + problem;
            }
        }
        else if (after.files[m].copy != member.copy)
        {
            return "changes the copy of file " + std::to_string(member.file) + " in " + name;
        }
    }

    return {};
}

// What is wrong with the group files beside the archive of `replaced`, the version of a file whose archive `built` was
// after file `file_id` took `bytes`, or nothing (GroupFileProblem). Copies that name the file's waves at other places
// count in `placed`.
std::string GroupFilesProblem(const sar::SoundArchive& built, const format::File& replaced, uint32_t file_id,
                              std::span<const uint8_t> bytes, std::size_t& placed)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::vector<std::vector<uint8_t>> group_files = format::ExportGroupFiles(replaced);
    if (group_files.size() != built.GroupFiles().size())
    {
        return "loses a group file";
    }
    for (std::size_t g = 0; g < group_files.size(); g++)
    {
        const std::string problem =
            GroupFileProblem(built.GroupFiles()[g], group_files[g], flavor, file_id, built.FileData(file_id), bytes,
                             "group file " + std::to_string(g), placed);
        if (!problem.empty())
        {
            return problem;
        }
    }

    return {};
}

// The groups that join for an item, and the name of the group among them that the windows don't propose, if any.
struct Choice
{
    std::vector<uint32_t> groups;
    std::string extra;
};

// The groups that join for a new or replaced item: those that the windows propose (edit::GroupOptions) for an item that
// requires the files `after` in the place of an item, or a model, that required `before`. With `extra`, a group that
// the windows don't propose joins too: one beside the archive where `file` holds one. `groups` are the groups of
// `built`, the archive that `file` builds. Counts the items without a proposal in `totals`, and the groups that the
// windows don't offer because they would take a file that Hokuto can't read.
Choice ChosenGroups(const format::File& file, const sar::SoundArchive& built, std::span<const edit::Group> groups,
                    std::span<const uint32_t> before, std::span<const uint32_t> after, bool extra, Totals& totals)
{
    const auto beside = [&file](uint32_t group)
    {
        return std::ranges::any_of(file.native->group_files,
                                   [group](const format::NativeGroupFile& each) { return each.group == group; });
    };

    const std::vector<edit::GroupOption> options =
        edit::GroupOptions(groups, before, after, edit::UnreadableFiles(built, after));
    const std::vector<edit::GroupOption> readable = edit::GroupOptions(groups, before, after);
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        totals.join_unreadable += readable[i].proposed && !options[i].proposed ? 1 : 0;
    }
    Choice chosen;
    const edit::GroupOption* other = nullptr;
    for (const edit::GroupOption& option : options)
    {
        if (option.proposed)
        {
            chosen.groups.push_back(option.group);
        }
        else if (extra && option.unavailable.empty() && !option.holds &&
                 (!other || (beside(option.group) && !beside(other->group))))
        {
            other = &option;
        }
    }
    totals.join_unproposed += chosen.groups.empty() ? 1 : 0;
    if (other)
    {
        chosen.groups.insert(std::ranges::upper_bound(chosen.groups, other->group), other->group);
        chosen.extra = other->name;
    }

    return chosen;
}

// What is wrong with `joined`, the version of `added` in which groups `chosen` load `items` (edit::JoinGroups), or
// nothing. `before` lists the files that the old item or the model required. Counts the groups, files and items that
// joined in `totals`.
std::string JoinProblem(const format::File& added, const format::File& joined, std::span<const uint32_t> chosen,
                        std::span<const uint32_t> items, std::span<const uint32_t> before, Totals& totals)
{
    const sar::Flavor flavor = added.native->flavor;
    const StrictRead was = ReadStrictly(added);
    const StrictRead now = ReadStrictly(joined);
    if (now.groups.size() != was.groups.size() || now.archive->FileCount() != was.archive->FileCount())
    {
        return "changes the number of groups or files";
    }

    // The files that each item requires, and the wave archives' files: a link to one leaves its list of groups.
    const auto has = [](std::span<const uint32_t> files, uint32_t file)
    {
        return std::ranges::find(files, file) != files.end();
    };

    std::vector<std::vector<uint32_t>> required;
    for (const uint32_t item : items)
    {
        required.push_back(edit::FilesOfItem(*now.archive, item));
    }
    std::set<uint32_t> wave_archives;
    for (const sar::WaveArchiveInfo& wave_archive : now.archive->WaveArchives())
    {
        wave_archives.insert(wave_archive.file_id);
    }

    // The files that a link can't reach: the file table gives them no place in the FILE block, or puts them beside the
    // archive.
    const sar::Tables tables_was = sar::TablesOf(was.bytes, flavor);
    const sar::InfoBlock info_was = sar::InfoBlock::Read(tables_was.info, flavor);
    std::set<uint32_t> unplaced;
    for (uint32_t id = 0; id < info_was.files.size(); id++)
    {
        const xsflib::Reader r(info_was.files[id], flavor == sar::Flavor::kCafe);
        if (r.U16(0) != kInternalFile || r.U32(r.U32(4) + 4) == 0xFFFFFFFF)
        {
            unplaced.insert(id);
        }
    }

    std::set<uint32_t> changed_files;                // the files of the chosen groups inside the archive
    std::map<uint32_t, std::vector<uint32_t>> named; // the groups that each file's list gains
    std::set<uint32_t> took_beside;                  // the chosen groups beside the archive that took something
    for (uint32_t g = 0; g < now.groups.size(); g++)
    {
        const std::string& group_name = now.archive->Groups()[g].name;
        if (!has(chosen, g))
        {
            if (now.groups[g] != was.groups[g])
            {
                return "changes group " + group_name + " outside the choice";
            }
            continue;
        }
        if (!was.groups[g] || !now.groups[g])
        {
            return "leaves group " + group_name + " unread";
        }

        // The group keeps its files and items first.
        const sar::GroupContents& old = *was.groups[g];
        const sar::GroupContents& contents = *now.groups[g];
        if (contents.files.size() < old.files.size() || contents.items.size() < old.items.size() ||
            !std::equal(old.files.begin(), old.files.end(), contents.files.begin()) ||
            !std::equal(old.items.begin(), old.items.end(), contents.items.begin()))
        {
            return "doesn't keep the files and items of group " + group_name + " first";
        }
        std::vector<uint32_t> held;
        for (const sar::GroupMember& member : contents.files)
        {
            held.push_back(member.file);
        }
        const std::span<const uint32_t> held_before = std::span<const uint32_t>(held).first(old.files.size());
        const bool loads = !before.empty() && has(held_before, before.front());

        // The group holds each file that an item requires, but for the files of the old item or the model where it
        // loads them. It takes no other file. It takes copies where it held copies. A group without files takes copies
        // when a link can't reach one of the files, and links otherwise.
        for (const std::vector<uint32_t>& files : required)
        {
            for (const uint32_t file : files)
            {
                if (!has(held, file) && !(loads && has(before, file)))
                {
                    return "leaves file " + std::to_string(file) + " out of group " + group_name;
                }
            }
        }
        const auto linkless = [&unplaced](const sar::GroupMember& member)
        {
            return unplaced.contains(member.file);
        };
        const bool copies =
            old.files.empty() ? std::ranges::any_of(contents.files, linkless) : old.files.front().copy.has_value();
        for (std::size_t m = old.files.size(); m < contents.files.size(); m++)
        {
            const sar::GroupMember& member = contents.files[m];
            const bool wanted = std::ranges::any_of(required, [&](const std::vector<uint32_t>& files)
                                                    { return has(files, member.file); }) &&
                                !(loads && has(before, member.file));
            if (!wanted || std::ranges::count(held, member.file) != 1)
            {
                return "puts file " + std::to_string(member.file) + " into group " + group_name + " for no item";
            }
            if (member.copy.has_value() != copies)
            {
                return "gives group " + group_name + (copies ? " a link to file " : " a copy of file ") +
                       std::to_string(member.file);
            }
            if (member.copy && !std::ranges::equal(*member.copy, now.archive->FileData(member.file)))
            {
                return "gives group " + group_name + " a copy of file " + std::to_string(member.file) +
                       " with other bytes";
            }
            (member.copy ? totals.join_copies : totals.join_links)++;
            if (member.copy || !wave_archives.contains(member.file))
            {
                named[member.file].push_back(sar::ItemId(sar::kGroupItem, g));
            }
        }

        // The group lists each item whose file it didn't hold and that it didn't list, once. The load flags name every
        // part exactly when the group holds every file that the item requires. A group's file without INFX lists none.
        std::size_t next = old.items.size();
        totals.join_unlisting += now.listing[g] ? 0 : 1;
        for (std::size_t i = 0; i < items.size() && now.listing[g]; i++)
        {
            const uint32_t item = items[i];
            const bool listed =
                std::ranges::any_of(old.items, [item](const sar::GroupItem& each) { return each.item == item; });
            if (listed || required[i].empty() || has(held_before, required[i].front()))
            {
                continue;
            }
            if (next >= contents.items.size() || contents.items[next].item != item)
            {
                return "doesn't list item " + HexOf(item) + " in group " + group_name;
            }
            const uint32_t load = contents.items[next++].load;
            const bool whole = std::ranges::all_of(required[i], [&](uint32_t file) { return has(held, file); });
            if (whole != (load == 0xFFFFFFFF) || (!whole && (load == 0 || (load & ~0xFu) != 0)))
            {
                return "lists item " + HexOf(item) + " in group " + group_name + " with the load flags " + HexOf(load);
            }
            totals.join_listed++;
            totals.join_partial += whole ? 0 : 1;
        }
        if (next != contents.items.size())
        {
            return "lists other items in group " + group_name;
        }

        const uint32_t group_file = now.archive->Groups()[g].file_id;
        if (group_file != sar::kExternalGroupFile)
        {
            changed_files.insert(group_file);
        }
        else if (contents != old)
        {
            took_beside.insert(g);
        }
        totals.join_groups++;
        totals.join_loading += loads ? 1 : 0;
        totals.join_beside += group_file == sar::kExternalGroupFile ? 1 : 0;
    }

    // The other files keep their bytes. Each file's list of groups gains the groups that took a copy of the file, or a
    // link to it but for a wave archive's file.
    for (uint32_t id = 0; id < now.archive->FileCount(); id++)
    {
        if (!changed_files.contains(id) && !std::ranges::equal(now.archive->FileData(id), was.archive->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    const std::vector<std::optional<std::vector<uint32_t>>> lists_was = ListsOfGroups(was.bytes, flavor);
    std::vector<std::optional<std::vector<uint32_t>>> lists_now = ListsOfGroups(now.bytes, flavor);
    for (uint32_t id = 0; id < lists_now.size() && id < lists_was.size(); id++)
    {
        std::optional<std::vector<uint32_t>> expected = lists_was[id];
        const auto gained = named.find(id);
        for (const uint32_t group : gained != named.end() ? gained->second : std::vector<uint32_t>())
        {
            if (!expected)
            {
                return "gives file " + std::to_string(id) + " a group without a list of groups";
            }

            if (!has(*expected, group))
            {
                expected->push_back(group);
            }
        }
        if (expected)
        {
            std::ranges::sort(*expected);
        }
        if (lists_now[id])
        {
            std::ranges::sort(*lists_now[id]);
        }
        if (lists_now[id] != expected)
        {
            return "changes the list of groups of file " + std::to_string(id);
        }
    }

    // The items' tables, the names and the other group files stay. An export writes each group file that changed.
    const sar::Tables tables_now = sar::TablesOf(now.bytes, flavor);
    const sar::InfoBlock info_now = sar::InfoBlock::Read(tables_now.info, flavor);
    if (info_now.sounds != info_was.sounds || info_now.sound_groups != info_was.sound_groups ||
        info_now.banks != info_was.banks || info_now.wave_archives != info_was.wave_archives ||
        info_now.groups != info_was.groups || info_now.players != info_was.players ||
        info_now.player_settings != info_was.player_settings ||
        tables_now.strings.has_value() != tables_was.strings.has_value() ||
        (tables_now.strings && !std::ranges::equal(*tables_now.strings, *tables_was.strings)))
    {
        return "changes the tables of the items";
    }
    const std::vector<format::ChangedGroupFile> changed = format::ChangedGroupFiles(joined);
    for (std::size_t k = 0; k < now.group_files.size(); k++)
    {
        const std::optional<uint32_t> group = joined.native->group_files[k].group;
        const bool took = group && took_beside.contains(*group);
        if (!took && now.group_files[k] != was.group_files[k])
        {
            return "changes group file " + std::to_string(k);
        }
        if (took &&
            std::ranges::none_of(changed, [k](const format::ChangedGroupFile& each) { return each.index == k; }))
        {
            return "leaves group file " + std::to_string(k) + " out of the export";
        }
    }

    // The file saves and opens again, and the same join again changes nothing.
    if (!hokuto::tools::SavesAndOpens(joined, true))
    {
        return "doesn't save and open again";
    }
    if (edit::JoinGroups(joined, chosen, items, before) != joined)
    {
        return "changes the groups when the items join them again";
    }

    return {};
}

// What is wrong when `items` of `added` join the groups of `choice` through the windows' edit, or nothing. `before`
// lists the files that the old item or the model required. Counts the join in `totals`.
std::string JoinCheck(const format::File& added, const Choice& choice, std::span<const uint32_t> items,
                      std::span<const uint32_t> before, Totals& totals)
{
    const std::span<const uint32_t> chosen = choice.groups;
    format::File joined;
    const auto start = std::chrono::steady_clock::now();
    try
    {
        joined = edit::JoinGroups(added, chosen, items, before);
    }
    catch (const std::invalid_argument& error)
    {
        // The window shows the edit's refusal of a link to a file that only groups hold, and of a file that Hokuto
        // can't read in the group that the windows don't propose.
        const std::string reason = error.what();
        const bool unreadable = !choice.extra.empty() && reason.starts_with("Group " + choice.extra + " would take") &&
                                reason.ends_with("and Hokuto can't read it.");
        if (!unreadable && reason.find("Hokuto can't link to it") == std::string::npos)
        {
            throw;
        }
        totals.join_refused++;
        return {};
    }
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    totals.join_slowest = std::max(totals.join_slowest, took.count());
    totals.join_items++;

    const std::string problem = JoinProblem(added, joined, chosen, items, before, totals);
    totals.join_failed += problem.empty() ? 0 : 1;

    return problem.empty() ? problem : "joins its groups and " + problem;
}

// A name for a new group that no item of `archive` holds: GROUP_CHECK, or GROUP_CHECK_2 and on.
std::string NewGroupName(const sar::SoundArchive& archive)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    const std::optional<std::span<const uint8_t>> strings = sar::TablesOf(archive.Bytes(), flavor).strings;
    std::string name = "GROUP_CHECK";
    for (int k = 2; strings && sar::FindItem(*strings, flavor, name); k++)
    {
        name = "GROUP_CHECK_" + std::to_string(k);
    }

    return name;
}

// What is wrong with `with_group`, the version of `added` with a new group named `name` (edit::AddGroups), or nothing.
// The new group follows the others, with an empty group's file at the end of the file table. The other groups and files
// keep their bytes, the group files beside the archive stay, and every item keeps its name. The game's sizes give the
// new group a size of 0. The file saves and opens again.
std::string NewGroupProblem(const format::File& added, const format::File& with_group, const std::string& name)
{
    const StrictRead was = ReadStrictly(added);
    const StrictRead now = ReadStrictly(with_group);
    const std::size_t groups = was.groups.size();
    const std::size_t files = was.archive->FileCount();
    if (now.groups.size() != groups + 1 || now.archive->FileCount() != files + 1)
    {
        return "adds other groups or files than the new group's";
    }
    const sar::GroupInfo& group = now.archive->Groups()[groups];
    if (group.name != name || group.file_id != files)
    {
        return "gives the new group another name or file";
    }

    // On the 3DS, the new group's file takes the highest version of the groups' files inside the archive.
    const sar::Flavor flavor = added.native->flavor;
    std::optional<uint32_t> version;
    for (uint32_t g = 0; g < groups && flavor == sar::Flavor::kCtr; g++)
    {
        const uint32_t file_id = was.archive->Groups()[g].file_id;
        const std::span<const uint8_t> bytes =
            file_id != sar::kExternalGroupFile ? was.archive->FileData(file_id) : std::span<const uint8_t>();
        if (bytes.size() >= 12)
        {
            version = std::max(version.value_or(0), xsflib::Reader(bytes, false).U32(8));
        }
    }
    const std::vector<uint8_t> empty = sar::EmptyGroupFile(flavor, version);
    if (!now.groups[groups] || *now.groups[groups] != sar::GroupContents() ||
        !std::ranges::equal(now.archive->FileData(group.file_id),
                            std::span(empty).first(sar::GroupSizeInArchive(empty, flavor))))
    {
        return "gives the new group another file than an empty group's";
    }
    for (uint32_t g = 0; g < groups; g++)
    {
        if (now.groups[g] != was.groups[g] || now.archive->Groups()[g].name != was.archive->Groups()[g].name)
        {
            return "changes group " + was.archive->Groups()[g].name;
        }
    }
    for (uint32_t id = 0; id < files; id++)
    {
        if (!std::ranges::equal(now.archive->FileData(id), was.archive->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    if (now.group_files != was.group_files)
    {
        return "changes the group files beside the archive";
    }

    // Every item keeps its name.
    const auto names = [](const auto& items)
    {
        std::vector<std::string> each;
        for (const auto& item : items)
        {
            each.push_back(item.name);
        }

        return each;
    };
    if (names(now.archive->Sounds()) != names(was.archive->Sounds()) ||
        names(now.archive->SoundGroups()) != names(was.archive->SoundGroups()) ||
        names(now.archive->Banks()) != names(was.archive->Banks()) ||
        names(now.archive->WaveArchives()) != names(was.archive->WaveArchives()) ||
        names(now.archive->Players()) != names(was.archive->Players()))
    {
        return "changes the names of the items";
    }

    // The game's sizes, and the saved file.
    const std::optional<format::GameSizes>& sizes = with_group.native->game_sizes;
    if (added.native->game_sizes &&
        (!sizes || sizes->archive != added.native->game_sizes->archive ||
         !std::ranges::equal(std::span(sizes->groups).first(groups), added.native->game_sizes->groups) ||
         sizes->groups.size() != groups + 1 || sizes->groups.back() != 0))
    {
        return "gives the new group a size among the game's sizes";
    }
    if (!hokuto::tools::SavesAndOpens(with_group, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// What is wrong when `items` of `added` join a new group through the windows' edit, or nothing. `archive` is the
// archive that `added` started from, and `before` lists the files that the old item or the model required. The new
// group goes in first, empty (edit::AddGroups). It then takes the items as any group without files does: links to the
// files that the items require, or copies when a link can't reach one of them. Counts the new group in `totals`.
std::string NewGroupCheck(const format::File& added, const sar::SoundArchive& archive, std::span<const uint32_t> items,
                          std::span<const uint32_t> before, Totals& totals)
{
    const std::vector<std::string> names = {NewGroupName(archive)};
    format::File joined;
    try
    {
        joined = edit::JoinGroups(added, {}, items, before, names);
    }
    catch (const std::invalid_argument& error)
    {
        // The window shows the edit's refusal of a file that Hokuto can't read, and of a name in an archive without
        // names.
        const std::string reason = error.what();
        const bool unreadable = reason.starts_with("Group " + names.front() + " would take") &&
                                reason.ends_with("and Hokuto can't read it.");
        if (!unreadable && reason != "The archive holds no names. Hokuto can't name a new group in it.")
        {
            throw;
        }
        totals.made_refused++;
        return {};
    }

    const format::File with_group = edit::AddGroups(added, names);
    std::string problem = NewGroupProblem(added, with_group, names.front());
    if (problem.empty())
    {
        Totals joins;
        const auto group = static_cast<uint32_t>(added.native->tables->groups.size());
        problem = JoinProblem(with_group, joined, std::span(&group, 1), items, before, joins);
        totals.made_copies += joins.join_copies;
        totals.made_links += joins.join_links;
        totals.made_listed += joins.join_listed;
        totals.made_unlisting += joins.join_unlisting;
    }
    totals.made_groups++;
    totals.made_failed += problem.empty() ? 0 : 1;

    return problem.empty() ? problem : "joins a new group and " + problem;
}

// The summary of the joins, for an archive with groups when `groups` says so, and of the new groups.
void PrintJoins(const std::string& name, const Totals& totals, bool groups)
{
    if (groups)
    {
        std::printf(
            "%s: %zu items or sets of new items joined %zu groups. %zu of the groups loaded the old item or the model, "
            "and %zu lie beside the archive. The groups took %zu copies and %zu links, and listed %zu items, %zu of "
            "them with some of their parts. %zu times, the group's file held no INFX block and listed no items. %zu "
            "joins fail, and the edit refused %zu. For %zu items, the windows propose no group. %zu groups that load "
            "the old item or the model would take a file that Hokuto can't read, and the windows don't offer them. The "
            "slowest join took %.2f s.\n",
            name.c_str(), totals.join_items, totals.join_groups, totals.join_loading, totals.join_beside,
            totals.join_copies, totals.join_links, totals.join_listed, totals.join_partial, totals.join_unlisting,
            totals.join_failed, totals.join_refused, totals.join_unproposed, totals.join_unreadable,
            totals.join_slowest);
    }
    std::printf(
        "%s: %zu new groups took items: %zu copies and %zu links, and %zu items listed. %zu new groups' files held no "
        "INFX block. %zu fail, and the edit refused %zu.\n",
        name.c_str(), totals.made_groups, totals.made_copies, totals.made_links, totals.made_listed,
        totals.made_unlisting, totals.made_failed, totals.made_refused);
}

// A sound's sequence file as Replace… reads it for an archive of `into`, and the item where the sound starts in it, if
// an item starts there.
struct Source
{
    edit::Incoming incoming;
    std::optional<uint32_t> start;
};

// The source that sequence file `bytes` gives for an archive of `into`, for a sound that starts at byte `start_offset`.
Source SourceOf(std::span<const uint8_t> bytes, uint32_t start_offset,
                const std::optional<song::Console>& into = std::nullopt)
{
    Source source{edit::ReadIncoming(bytes, into), std::nullopt};
    source.start = edit::ItemAtOffset(
        edit::Offsets(source.incoming.sequence, song::Console{.flavor = source.incoming.flavor}), start_offset);

    return source;
}

// Whether the commands that sound `info` plays from `source` open tracks that the sound doesn't allocate.
bool Untracked(const Source& source, const sar::SoundInfo& info)
{
    return (edit::TracksOpened(source.incoming.sequence, *source.start) & ~info.allocate_track_flags) != 0;
}

// `played` without the places of its commands: the commands' offsets, the stretches, the labels and the calls.
song::Sequence Placeless(song::Sequence played)
{
    played.name.clear();
    played.sound = 0;
    played.labels.clear();
    played.calls.clear();
    for (song::Track& track : played.tracks)
    {
        track.stretches.clear();
        for (song::Note& note : track.notes)
        {
            note.command = 0;
            note.stretch = 0;
        }
    }

    return played;
}

// What ntdlib's performer plays of sound `sound` of `archive`, with the variables' starting values `values`, without
// the places of its commands.
song::Sequence Played(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values = {})
{
    return Placeless(song::ReadSequence(archive, sound, values));
}

// Whether the sequence file of sound `sound` of `archive` has a label at the sound's start.
bool StartsAtLabel(const sar::SoundArchive& archive, uint32_t sound)
{
    const sar::SoundInfo& info = archive.Sounds()[sound];
    const std::vector<sar::Sequence::Label> labels = sar::SequenceFile::Read(archive.FileData(info.file_id)).labels;
    const auto at_start = [&info](const sar::Sequence::Label& label)
    {
        return label.offset == info.start_offset;
    };

    return std::ranges::any_of(labels, at_start);
}

// Whether `after` plays the notes of `before` where ntdlib's performer finds their loops a whole number of passes
// apart. The performer finds a loop where every track's state repeats. Up to the earlier loop's end, the notes then
// have to start as before, with their keys, velocities and instruments.
bool SameUpToLoop(const song::Sequence& before, const song::Sequence& after)
{
    if (!before.loop || !after.loop || before.loop == after.loop)
    {
        return false;
    }

    const uint32_t period = before.loop->end - before.loop->start;
    const uint32_t shift = after.loop->start > before.loop->start ? after.loop->start - before.loop->start
                                                                  : before.loop->start - after.loop->start;
    if (period == 0 || after.loop->end - after.loop->start != period || shift % period != 0)
    {
        return false;
    }

    const uint32_t end = std::min(before.loop->end, after.loop->end);
    const auto starts = [end](const song::Sequence& sequence)
    {
        std::vector<std::tuple<int, uint32_t, uint8_t, uint8_t, uint8_t, uint32_t>> notes;
        for (const song::Track& track : sequence.tracks)
        {
            for (const song::Note& note : track.notes)
            {
                if (note.start < end)
                {
                    notes.emplace_back(track.number, note.start, note.key, note.velocity, note.bank_slot, note.program);
                }
            }
        }
        std::ranges::sort(notes);

        return notes;
    };

    return starts(after) == starts(before);
}

// What is wrong when sound `sound` of `file` takes sound `other`'s sequence file, from `source`, and bank slots, or
// nothing. `built` is the archive that `file` builds, with `groups`. `before` holds what the performer plays of each
// sound there. `replaced` takes the new version of `file`. A sound whose loop the performer finds a whole number of
// passes away counts in `moved` (SameUpToLoop).
std::string CrossProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                         const sar::SoundArchive& built, std::span<const Group> groups, uint32_t sound, uint32_t other,
                         const Source& source, std::map<uint32_t, song::Sequence>& before,
                         std::optional<format::File>& replaced, std::size_t& moved)
{
    // The replacement, with the other sound's bank slots for each sound that plays the new commands.
    const uint32_t tracks = edit::TracksToAdd(file, places, sound, source.incoming.sequence, *source.start);
    replaced = edit::ReplaceSequence(file, places, sound, source.incoming.sequence, *source.start, {}, "Check", tracks,
                                     built.Sounds()[other].banks);
    std::vector<uint32_t> playing = edit::SharingStart(places, sound);
    playing.push_back(sound);
    hokuto::tools::CheckSaved(*replaced, true);
    const Archive after =
        song::LoadArchive(format::ExportArchive(*replaced), "after.bfsar", format::ExportGroupFiles(*replaced));

    // The sounds that play the new commands play the other sound's notes. The file's other sounds play as before.
    const auto played_before = [&](uint32_t which) -> const song::Sequence&
    {
        auto found = before.find(which);
        if (found == before.end())
        {
            found = before.emplace(which, Played(built, which)).first;
        }

        return found->second;
    };
    for (const uint32_t player : playing)
    {
        const song::Sequence played = Played(*after, player);
        if (played != played_before(other))
        {
            if (!SameUpToLoop(played_before(other), played))
            {
                return "doesn't play " + built.Sounds()[other].name + "'s notes from its file";
            }
            moved++;
        }
    }
    for (const uint32_t sharing : edit::SharingFile(places, sound))
    {
        if (std::ranges::find(playing, sharing) == playing.end() && Played(*after, sharing) != played_before(sharing))
        {
            return "changes how " + built.Sounds()[sharing].name + " plays";
        }
    }

    // The other files keep their bytes, and the other sounds their starts. A group's file can hold a copy of the
    // sound's file.
    const uint32_t file_id = built.Sounds()[sound].file_id;
    std::vector<uint32_t> copies = {file_id};
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, file_id) != group.loads.end())
        {
            copies.push_back(group.file_id);
        }
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (std::ranges::find(copies, id) == copies.end() &&
            !std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    for (std::size_t i = 0; i < built.Sounds().size(); i++)
    {
        const bool plays = std::ranges::find(playing, static_cast<uint32_t>(i)) != playing.end();
        if (!plays && after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            return "moves the start of " + built.Sounds()[i].name;
        }
    }

    // Each copy in a group's file inside the archive, and in the group files beside it, holds the new bytes.
    std::size_t placed = 0;
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, file_id) == group.loads.end() || built.FileData(group.file_id).empty())
        {
            continue;
        }

        const std::string problem = GroupFileProblem(built.FileData(group.file_id), after->FileData(group.file_id),
                                                     built.ReadOptions().flavor, file_id, built.FileData(file_id),
                                                     after->FileData(file_id), "group " + HexOf(group.file_id), placed);
        if (!problem.empty())
        {
            return problem;
        }
    }

    return GroupFilesProblem(built, *replaced, file_id, after->FileData(file_id), placed);
}

// What is wrong when sound `sound` of `file` takes its sequence file again, from the archive and for each other
// console, or nothing. `built` is the archive that `file` builds. A file that the 3DS can't hold counts apart.
std::string SameProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                        const sar::SoundArchive& built, uint32_t sound, const Source& same, Totals& totals)
{
    const sar::SoundInfo& info = built.Sounds()[sound];
    if (!same.start)
    {
        return "starts where no command of its file starts";
    }

    const auto replaced = [&](const Source& source)
    {
        const uint32_t tracks = edit::TracksToAdd(file, places, sound, source.incoming.sequence, *source.start);
        return edit::ReplaceSequence(file, places, sound, source.incoming.sequence, *source.start, {}, "Check", tracks);
    };

    const auto offered = [&](const Source& source)
    {
        const std::vector<edit::StartingSound> starts = edit::SoundStarts(file, places, source.incoming.sequence);
        return std::ranges::find(starts, edit::StartingSound{sound, *source.start}) != starts.end();
    };
    if (!offered(same))
    {
        return "isn't offered its start in its file";
    }
    if (replaced(same) != file)
    {
        return "doesn't come back from its file";
    }

    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::span<const uint8_t> bytes = built.FileData(info.file_id);
    std::vector<uint32_t> starts = {info.start_offset};
    for (const uint32_t sharing : song::SoundsSharingFile(built, sound))
    {
        starts.push_back(built.Sounds()[sharing].start_offset);
    }
    for (const sar::Flavor console : song::OtherConsoles(flavor))
    {
        std::vector<uint8_t> other_file;
        try
        {
            other_file = song::OtherConsoleSequenceFile(built, sound, console).file;
        }
        catch (const std::runtime_error&)
        {
            if (console != sar::Flavor::kCtr)
            {
                throw;
            }
            totals.ctr_refused++;
            continue;
        }

        const std::string name = song::ConsoleName(console);
        const Source other = SourceOf(other_file, info.start_offset, song::Console{.flavor = flavor});
        if (!other.start)
        {
            return "starts where no command of its file for the " + name + " starts";
        }
        if (replaced(other) == file)
        {
            if (!offered(other))
            {
                return "isn't offered its start in its file for the " + name;
            }
            continue;
        }

        // The Switch's biquad filters of types 1 to 5 take other values for another console, and come back as types 6
        // to 10 (song::MapBiquads).
        if (other.incoming.sequence != same.incoming.sequence &&
            song::SameButBiquads(other.incoming.sequence, same.incoming.sequence))
        {
            if (!offered(other))
            {
                return "isn't offered its start in its file for the " + name;
            }

            totals.other_biquads++;
            continue;
        }

        // Bytes that no command reaches can come back otherwise from another console. Bytes 8 to 11 hold the version.
        std::vector<uint8_t> back = song::ConvertSequenceFile(other_file, console, flavor, starts).file;
        std::copy(bytes.begin() + 8, bytes.begin() + 12, back.begin() + 8);
        if (std::ranges::equal(back, bytes))
        {
            return "doesn't come back from its file for the " + name;
        }
        totals.other_unreached++;
    }

    return {};
}

// A file that Replace… replaces: its kind, the item whose context menu offers it, and its file ID. A wave sound file is
// the file of a wave sound.
struct FileItem
{
    edit::FileKind kind = edit::FileKind::kBank;
    uint32_t item = 0;
    uint32_t file_id = 0;
};

// The banks' files, the wave archives' files and the wave sound files of `archive`, each file once, in that order.
std::vector<FileItem> FileItemsOf(const sar::SoundArchive& archive)
{
    std::vector<FileItem> items;
    std::set<std::pair<edit::FileKind, uint32_t>> seen;
    const auto add = [&](edit::FileKind kind, std::size_t item, uint32_t file_id)
    {
        if (seen.insert({kind, file_id}).second)
        {
            items.push_back({kind, static_cast<uint32_t>(item), file_id});
        }
    };
    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        add(edit::FileKind::kBank, i, archive.Banks()[i].file_id);
    }
    for (std::size_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        add(edit::FileKind::kWaveArchive, i, archive.WaveArchives()[i].file_id);
    }
    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        if (archive.Sounds()[i].type == sar::SoundType::kWaveSound)
        {
            add(edit::FileKind::kWaveSoundFile, i, archive.Sounds()[i].file_id);
        }
    }

    return items;
}

// What is wrong with `replaced`, the version of a file whose archive holds `bytes` as the file of `item`, or nothing.
// `built` is the archive that the old version builds, with `groups`. `after` takes the archive that `replaced` builds.
// Each copy in a group holds what CopyProblem requires. The copies that named the file's waves at other places count in
// `counted`, where the call gives it.
std::string ReplacedProblem(const sar::SoundArchive& built, std::span<const Group> groups, const FileItem& item,
                            std::span<const uint8_t> bytes, const format::File& replaced, Archive& after,
                            std::size_t* counted = nullptr)
{
    hokuto::tools::CheckSaved(replaced, true);
    std::size_t uncounted = 0;
    std::size_t& placed = counted ? *counted : uncounted;
    after = song::LoadArchive(format::ExportArchive(replaced), "after.bfsar", format::ExportGroupFiles(replaced));
    if (!std::ranges::equal(after->FileData(item.file_id), bytes))
    {
        return "doesn't hold the new bytes";
    }

    // Each copy in a group's file inside the archive holds what CopyProblem requires.
    const sar::Flavor flavor = built.ReadOptions().flavor;
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, item.file_id) == group.loads.end() || built.FileData(group.file_id).empty())
        {
            continue;
        }

        const std::string problem =
            GroupFileProblem(built.FileData(group.file_id), after->FileData(group.file_id), flavor, item.file_id,
                             built.FileData(item.file_id), bytes, "group " + HexOf(group.file_id), placed);
        if (!problem.empty())
        {
            return problem;
        }
    }

    // The other files keep their bytes, and the sounds their starts. A group's file can hold a copy of the file.
    std::vector<uint32_t> copies = {item.file_id};
    for (const Group& group : groups)
    {
        if (std::ranges::find(group.loads, item.file_id) != group.loads.end())
        {
            copies.push_back(group.file_id);
        }
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (std::ranges::find(copies, id) == copies.end() &&
            !std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    for (std::size_t i = 0; i < built.Sounds().size(); i++)
    {
        if (after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            return "moves the start of " + built.Sounds()[i].name;
        }
    }

    // Each copy of the file in the group files beside the archive holds the new bytes, with the old copy's places where
    // it named the file's waves at other places.
    return GroupFilesProblem(built, replaced, item.file_id, bytes, placed);
}

// What is wrong when the file of `item` takes `bytes`, the bytes of another file of its kind, or nothing. `built` is
// the archive that `file` builds, with `groups`. `after` takes the archive that the new version of `file` builds. The
// copies in groups that named the file's waves at other places count in `placed`, where the call gives it.
std::string FileProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                        const FileItem& item, std::span<const uint8_t> bytes, Archive& after,
                        std::size_t* placed = nullptr)
{
    // The Replace window reads the file with the versions of the present file, and finds what it changes. Then the
    // replacement.
    const edit::IncomingFile incoming = edit::ReadIncomingFile(bytes, item.kind, built.ReadOptions().flavor,
                                                               edit::IncomingVersions(built, item.kind, item.item));
    edit::ChangesOf(file, built, item.item, incoming);
    const format::File replaced = edit::ReplaceFile(file, item.file_id, incoming.bytes);
    return ReplacedProblem(built, groups, item, incoming.bytes, replaced, after, placed);
}

// The instrument at program `program` of `bank`, or nothing.
const sar::Instrument* InstrumentAt(const sar::Bank& bank, uint32_t program)
{
    return program < bank.instruments.size() && bank.instruments[program] ? &*bank.instruments[program] : nullptr;
}

// Whether ntdlib reads the same instrument at program `p` of `a` as at program `q` of `b`, or neither: the same key and
// velocity regions with the same values, and each region's wave by its wave archive and index.
bool SameInstrument(const sar::Bank& a, uint32_t p, const sar::Bank& b, uint32_t q)
{
    const sar::Instrument* x = InstrumentAt(a, p);
    const sar::Instrument* y = InstrumentAt(b, q);
    if (!x || !y)
    {
        return !x && !y;
    }

    const auto same_region = [&a, &b](const sar::VelocityRegion& r, const sar::VelocityRegion& s)
    {
        const bool waves = r.wave_id_index < a.waves.size() && s.wave_id_index < b.waves.size() &&
                           a.waves[r.wave_id_index].wave_archive_item == b.waves[s.wave_id_index].wave_archive_item &&
                           a.waves[r.wave_id_index].index == b.waves[s.wave_id_index].index;

        const auto envelope = [](const sar::Adshr& e)
        {
            return std::array<uint8_t, 5>{e.attack, e.decay, e.sustain, e.hold, e.release};
        };

        return waves && r.original_key == s.original_key && r.volume == s.volume && r.pan == s.pan &&
               r.pitch == s.pitch && r.ignore_note_off == s.ignore_note_off && r.key_group == s.key_group &&
               r.interpolation == s.interpolation && envelope(r.adshr) == envelope(s.adshr);
    };

    if (x->keys.size() != y->keys.size())
    {
        return false;
    }
    for (std::size_t k = 0; k < x->keys.size(); k++)
    {
        const sar::KeyRegion& i = x->keys[k];
        const sar::KeyRegion& j = y->keys[k];
        if (i.lo != j.lo || i.hi != j.hi || i.velocities.size() != j.velocities.size())
        {
            return false;
        }
        for (std::size_t v = 0; v < i.velocities.size(); v++)
        {
            const sar::KeyRegion::Vel& m = i.velocities[v];
            const sar::KeyRegion::Vel& n = j.velocities[v];
            if (m.lo != n.lo || m.hi != n.hi || m.region.has_value() != n.region.has_value() ||
                (m.region && !same_region(*m.region, *n.region)))
            {
                return false;
            }
        }
    }

    return true;
}

// What is wrong when bank `item` takes `bytes`, the next bank, with copies of instruments at the programs that the
// sounds select and the next bank lacks: first the present bank's instruments, then the next bank's first instrument.
// `places` gives where each sound plays. A bank whose sounds select no such program gives nothing.
std::string FixesProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                         std::span<const std::optional<edit::SoundPlace>> places, const FileItem& item,
                         std::span<const uint8_t> bytes, Totals& totals)
{
    // The programs that the sounds select and the next bank lacks.
    const sar::Options& options = built.ReadOptions();
    const edit::IncomingFile incoming =
        edit::ReadIncomingFile(bytes, item.kind, options.flavor, edit::IncomingVersions(built, item.kind, item.item));
    const sar::Bank next = sar::Bank::Parse(incoming.bytes, options);
    const sar::Bank present = sar::Bank::Parse(built.FileData(item.file_id), options);
    const edit::BankSelections selections = edit::ProgramsOfBank(file, built, places, item.item);
    std::vector<uint32_t> lacked;
    for (const uint32_t program : selections.programs)
    {
        if (!InstrumentAt(next, program))
        {
            lacked.push_back(program);
        }
    }
    totals.selected += selections.programs.size();
    totals.undecided += selections.undecided;
    if (lacked.empty())
    {
        return {};
    }
    totals.fixed_banks++;
    totals.fixed_programs += lacked.size();

    // The present bank's instruments where it holds them, then the next bank's first instrument.
    std::optional<uint32_t> first;
    for (uint32_t program = 0; program < next.instruments.size() && !first; program++)
    {
        first = next.instruments[program] ? std::optional<uint32_t>(program) : std::nullopt;
    }
    for (const bool from_present : {true, false})
    {
        std::vector<edit::ProgramFix> fixes;
        for (const uint32_t program : lacked)
        {
            if (from_present && InstrumentAt(present, program))
            {
                fixes.push_back({program, std::nullopt});
            }
            else if (!from_present && first)
            {
                fixes.push_back({program, *first});
            }
        }
        if (fixes.empty())
        {
            continue;
        }

        // The copies pass the checks of the next file. Each reads as its instrument, and the others as before.
        const std::vector<uint8_t> fixed = edit::WithProgramFixes(built, item.item, incoming.bytes, fixes);
        Archive after;
        const std::string problem =
            ReplacedProblem(built, groups, item, fixed, edit::ReplaceFile(file, item.file_id, fixed), after);
        if (!problem.empty())
        {
            return problem + " with copies of instruments";
        }
        const sar::Bank read = sar::Bank::Parse(after->FileData(item.file_id), options);
        for (uint32_t program = 0; program < std::max(read.instruments.size(), next.instruments.size()); program++)
        {
            const auto fix = std::ranges::find(fixes, program, &edit::ProgramFix::program);
            const bool same = fix == fixes.end() ? SameInstrument(read, program, next, program)
                              : fix->from        ? SameInstrument(read, program, next, *fix->from)
                                                 : SameInstrument(read, program, present, program);
            if (!same)
            {
                return "reads program " + std::to_string(program) + " otherwise after the copies of instruments";
            }
        }
        (from_present ? totals.fixes_present : totals.fixes_next) += fixes.size();
    }

    return {};
}

// The name of `item` in the tool's lines.
std::string NameOf(const sar::SoundArchive& archive, const FileItem& item)
{
    switch (item.kind)
    {
    case edit::FileKind::kBank:
        return archive.Banks()[item.item].name;
    case edit::FileKind::kWaveArchive:
        return archive.WaveArchives()[item.item].name;
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return "the wave sound file of " + archive.Sounds()[item.item].name;
}

// Whether another file ID of `archive` names the bytes of file `file_id` in the archive.
bool SharedFile(const sar::SoundArchive& archive, uint32_t file_id)
{
    const std::optional<std::size_t> offset = archive.FileOffset(file_id);
    for (uint32_t id = 0; id < archive.FileCount() && offset; id++)
    {
        if (id != file_id && archive.FileOffset(id) == offset &&
            archive.FileData(id).size() == archive.FileData(file_id).size())
        {
            return true;
        }
    }

    return false;
}

// Whether neither `archive` nor its group files hold file `file_id`: Replace… can't reach it.
bool Outside(const sar::SoundArchive& archive, uint32_t file_id)
{
    return archive.FileData(file_id).empty();
}

// Whether only the group files beside `archive` hold file `file_id`.
bool Beside(const sar::SoundArchive& archive, uint32_t file_id)
{
    return !Outside(archive, file_id) && !archive.FileOffset(file_id);
}

// The item of the browser whose file is the file of `item`.
song::Item SongItemOf(const FileItem& item)
{
    switch (item.kind)
    {
    case edit::FileKind::kBank:
        return {song::ItemKind::kBank, item.item, 0};
    case edit::FileKind::kWaveArchive:
        return {song::ItemKind::kWaveArchive, item.item, 0};
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return {song::ItemKind::kWaveSound, item.item, 0};
}

// The ID of the item whose file is the file of `item`. A wave sound file's item is a wave sound that plays it.
uint32_t ItemIdOf(const FileItem& item)
{
    switch (item.kind)
    {
    case edit::FileKind::kBank:
        return sar::ItemId(sar::kBankItem, item.item);
    case edit::FileKind::kWaveArchive:
        return sar::ItemId(sar::kWaveArchiveItem, item.item);
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return sar::ItemId(sar::kSoundItem, item.item);
}

// Whether two wave archives that ntdlib reads with `options` hold waves that decode to the same samples at the same
// rate, with the same loop.
bool SameWaveArchive(std::span<const uint8_t> a, std::span<const uint8_t> b, const sar::Options& options)
{
    const std::vector<std::span<const uint8_t>> x = sar::WaveArchive::Parse(a, options).waves;
    const std::vector<std::span<const uint8_t>> y = sar::WaveArchive::Parse(b, options).waves;
    if (x.size() != y.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < x.size(); i++)
    {
        const sar::Pcm p = sar::DecodeWave(x[i], options);
        const sar::Pcm q = sar::DecodeWave(y[i], options);
        if (p.sample_rate != q.sample_rate || p.loop != q.loop || p.loop_start != q.loop_start ||
            p.loop_end != q.loop_end || p.channels != q.channels)
        {
            return false;
        }
    }

    return true;
}

// Checks each file of a bank, a wave archive or a wave sound with itself, then with its file for each other console,
// then with the next file of its kind. A bank then takes copies of instruments at the programs that its sounds select
// and the next bank lacks (FixesProblem). `places` gives where each sound plays. After the replacement with the next
// file, the item joins the groups that the Replace window proposes (edit_groups), up to kJoins times.
void CheckFiles(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
                const std::string& name, Totals& totals)
{
    const std::vector<FileItem> items = FileItemsOf(built);
    std::size_t joins = 0;
    bool new_group = false;
    for (std::size_t i = 0; i < items.size(); i++)
    {
        // A file that neither the archive nor its group files hold, and a file that two file IDs name, count apart.
        const FileItem& item = items[i];
        const std::span<const uint8_t> bytes = built.FileData(item.file_id);
        if (Outside(built, item.file_id))
        {
            totals.files_outside++;
            continue;
        }
        if (SharedFile(built, item.file_id))
        {
            totals.files_shared++;
            continue;
        }
        totals.files_beside += Beside(built, item.file_id) ? 1 : 0;

        std::string problem;
        try
        {
            // A group's copy with other bytes takes the file's bytes. That copy changes.
            const song::FileVersions versions = edit::IncomingVersions(built, item.kind, item.item);
            const edit::IncomingFile same =
                edit::ReadIncomingFile(bytes, item.kind, built.ReadOptions().flavor, versions);
            const edit::FileChanges changes = edit::ChangesOf(file, built, item.item, same);
            totals.files_other_copies += changes.other_copies > 0 ? 1 : 0;
            totals.files_placed += changes.placed_copies > 0 ? 1 : 0;
            if (edit::ReplaceFile(file, item.file_id, same.bytes) != file && changes.other_copies == 0)
            {
                problem = "doesn't come back from its file";
            }

            // The files that Export… writes for the other consoles. Each goes in converted, with the versions of the
            // present file. The file has to come back unchanged too, but for a wave archive whose waves come back with
            // other bytes. Those waves have to decode as before.
            for (const sar::Flavor console : song::OtherConsoles(built.ReadOptions().flavor))
            {
                const std::vector<uint8_t> other_file = song::OtherConsoleFile(built, SongItemOf(item), console);
                const edit::IncomingFile converted =
                    edit::ReadIncomingFile(other_file, item.kind, built.ReadOptions().flavor, versions);
                edit::ChangesOf(file, built, item.item, converted);
                const bool same_bytes = std::ranges::equal(converted.bytes, bytes);
                if (!same_bytes && (item.kind != edit::FileKind::kWaveArchive ||
                                    !SameWaveArchive(converted.bytes, bytes, built.ReadOptions())))
                {
                    problem = "doesn't come back from the " + song::ConsoleName(console);
                }
                if (same_bytes && edit::ReplaceFile(file, item.file_id, converted.bytes) != file &&
                    changes.other_copies == 0)
                {
                    problem = "doesn't come back from the " + song::ConsoleName(console);
                }
                totals.files_converted_otherwise += same_bytes ? 0 : 1;
                totals.files_converted++;
            }

            // The next file of the same kind that the archive or its group files hold.
            for (std::size_t step = 1; step < items.size() && problem.empty(); step++)
            {
                const FileItem& other = items[(i + step) % items.size()];
                const std::span<const uint8_t> other_bytes = built.FileData(other.file_id);
                if (other.kind != item.kind || other_bytes.empty())
                {
                    continue;
                }

                Archive after;
                problem = FileProblem(file, built, groups, item, other_bytes, after, &totals.files_placed_copies);
                const edit::IncomingFile read =
                    edit::ReadIncomingFile(other_bytes, item.kind, built.ReadOptions().flavor, versions);
                totals.files_written += read.converted ? 1 : 0;
                if (problem.empty() && item.kind == edit::FileKind::kBank)
                {
                    problem = FixesProblem(file, built, groups, places, item, other_bytes, totals);
                }
                totals.files_crossed++;

                // The groups that load the item and lack a wave archive that the new file names take it. The first item
                // also joins a new group.
                const bool joining = !edit_groups.empty() && joins < kJoins;
                if (problem.empty() && (joining || !new_group))
                {
                    const uint32_t id = ItemIdOf(item);
                    const edit::IncomingFile incoming =
                        edit::ReadIncomingFile(other_bytes, item.kind, built.ReadOptions().flavor, versions);
                    const std::vector<uint32_t> files_before = edit::FilesOfItem(built, id);
                    const Choice chosen = joining ? ChosenGroups(file, built, edit_groups, files_before,
                                                                 edit::FilesOfItem(built, id, {.file = incoming.bytes}),
                                                                 joins == 0, totals)
                                                  : Choice();
                    if (!chosen.groups.empty() || !new_group)
                    {
                        const format::File replaced = edit::ReplaceFile(file, item.file_id, incoming.bytes);
                        if (!chosen.groups.empty())
                        {
                            joins++;
                            problem = JoinCheck(replaced, chosen, std::span(&id, 1), files_before, totals);
                        }
                        if (problem.empty() && !new_group)
                        {
                            new_group = true;
                            problem = NewGroupCheck(replaced, built, std::span(&id, 1), files_before, totals);
                        }
                    }
                }
                break;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), NameOf(built, item).c_str(), problem.c_str());
            totals.files_failed++;
        }
        totals.files++;
    }
}

// The version in the header of `file`, a file of the console of `flavor`.
uint32_t HeaderVersion(std::span<const uint8_t> file, sar::Flavor flavor)
{
    return xsflib::Reader(file, flavor == sar::Flavor::kCafe).U32(0x08);
}

// Whether `a` and `b` hold the same samples, rate and loop.
bool SamePcm(const sar::Pcm& a, const sar::Pcm& b)
{
    return a.sample_rate == b.sample_rate && a.loop == b.loop && a.loop_start == b.loop_start &&
           a.loop_end == b.loop_end && a.channels == b.channels;
}

// The samples that the console decodes in a DSP-ADPCM channel's loop from the loop's second pass on. Each such pass
// starts from the loop's context: the two samples before the loop's start, and a frame's predictor and scale. The
// Switch reads those from the frame's header when the loop starts at a frame's start. The Wii U takes them from the
// context there too.
std::vector<int16_t> LaterPass(std::span<const uint8_t> samples, const sar::DspAdpcmInfo& adpcm, uint32_t loop_start,
                               uint32_t loop_end, sar::Flavor flavor)
{
    std::vector<int16_t> pass;
    int32_t header = adpcm.loop_pred_scale & 0x7F;
    int32_t yn1 = adpcm.loop_yn1;
    int32_t yn2 = adpcm.loop_yn2;
    for (uint32_t i = loop_start; i < loop_end; i++)
    {
        // Each frame of 8 bytes holds a header and 14 samples of 4 bits.
        const std::size_t frame = std::size_t{i} / 14 * 8;
        const uint32_t place = i % 14;
        if (place == 0 && (i != loop_start || flavor == sar::Flavor::kNx))
        {
            header = samples[frame];
        }
        const uint8_t pair = samples[frame + 1 + place / 2];
        const int32_t nibble = place % 2 == 0 ? pair >> 4 : pair & 0xF;
        const int32_t predictor = (header >> 4) & 7;
        const int16_t sample =
            ntdlib::wave::DspAdpcmSample(nibble >= 8 ? nibble - 16 : nibble, header & 0xF, adpcm.coefs[2 * predictor],
                                         adpcm.coefs[2 * predictor + 1], yn1, yn2, ntdlib::wave::DspAdpcmSum::kWrap);
        pass.push_back(sample);
        yn2 = yn1;
        yn1 = sample;
    }

    return pass;
}

// Counts how the later passes of a looped DSP-ADPCM wave, `wave`, differ from its first pass, `pcm`. A WAV file holds
// the first pass, and a wave from it plays the first pass's samples in every pass.
void CountLaterPasses(std::span<const uint8_t> wave, const sar::Pcm& pcm, sar::Flavor flavor, Totals& totals)
{
    const sar::WaveFile parsed = sar::WaveFile::Read(wave);
    if (parsed.encoding != sar::WaveEncoding::kDspAdpcm || !pcm.loop)
    {
        return;
    }

    int peak = 0;
    uint32_t last = 0;
    for (std::size_t c = 0; c < pcm.channels.size(); c++)
    {
        const std::vector<int16_t> later =
            LaterPass(parsed.channels[c].samples, *parsed.channels[c].adpcm, pcm.loop_start, pcm.loop_end, flavor);
        for (std::size_t i = 0; i < later.size(); i++)
        {
            const int difference = std::abs(later[i] - pcm.channels[c][pcm.loop_start + i]);
            if (difference > 0)
            {
                peak = std::max(peak, difference);
                last = std::max(last, static_cast<uint32_t>(i + 1));
            }
        }
    }
    if (peak > 0)
    {
        totals.waves_later++;
        totals.later_peak = std::max(totals.later_peak, peak);
        totals.later_ms = std::max(totals.later_ms, 1000.0 * last / pcm.sample_rate);
    }
}

// What is wrong when wave `wave` of wave archive `item` comes back from its WAV file `wav` as DSP-ADPCM, or when the
// WAV file goes in as a new wave after the wave archive's waves without `wave`. `present` is the wave archive, and
// `pcm` holds the WAV file's samples. A loop's start moves forward to a frame, and the samples from the old start
// repeat after the loop's end, as often as it takes. The new wave has to play those samples closely, with the ratio of
// signal to noise that the Replace window shows. Each later pass of its loop has to decode to the first pass's samples.
// The new wave archive has to keep the other waves' bytes.
std::string DspAdpcmProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                            std::optional<uint32_t> wave, std::span<const uint8_t> wav, const sar::Pcm& pcm,
                            Totals& totals)
{
    const auto start = std::chrono::steady_clock::now();
    const edit::IncomingWave incoming =
        edit::ReadIncomingWave(wav, built, item.item, wave, sar::WaveEncoding::kDspAdpcm);
    totals.adpcm_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const uint32_t moved = pcm.loop ? (14 - pcm.loop_start % 14) % 14 : 0;
    if (incoming.encoding != sar::WaveEncoding::kDspAdpcm || incoming.loop_moved != moved ||
        incoming.frames != pcm.loop_end || incoming.loop_start != pcm.loop_start)
    {
        return "doesn't come back from its WAV file as DSP-ADPCM";
    }

    // The samples that the new wave plays up to its loop's end.
    std::vector<std::vector<int16_t>> played = pcm.channels;
    for (std::vector<int16_t>& channel : played)
    {
        for (std::size_t i = pcm.loop_start; i < pcm.loop_start + moved; i++)
        {
            const int16_t sample = channel[i];
            channel.push_back(sample);
        }
    }
    const sar::Pcm back = sar::DecodeWave(incoming.bytes, song::ItemOptions(built));
    if (back.sample_rate != pcm.sample_rate || back.loop != pcm.loop || back.loop_start != pcm.loop_start + moved ||
        back.loop_end != pcm.loop_end + moved || back.channels.size() != played.size())
    {
        return "comes back from its WAV file as DSP-ADPCM with another rate, loop or length";
    }

    // The Replace window's ratio of signal to noise over every channel.
    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t c = 0; c < played.size(); c++)
    {
        if (back.channels[c].size() != played[c].size())
        {
            return "comes back from its WAV file as DSP-ADPCM with another length";
        }

        for (std::size_t i = 0; i < played[c].size(); i++)
        {
            const double sample = played[c][i];
            const double error = sample - back.channels[c][i];
            signal += sample * sample;
            noise += error * error;
        }
    }
    const std::optional<double> ratio =
        noise > 0.0 ? std::optional<double>(10.0 * std::log10(signal / noise)) : std::nullopt;
    if (ratio.has_value() != incoming.signal_to_noise.has_value() ||
        (ratio && std::abs(*ratio - *incoming.signal_to_noise) > 1e-6))
    {
        return "as DSP-ADPCM has another ratio of signal to noise than the Replace window shows";
    }

    // Each later pass of the loop starts from the loop's context and plays the first pass's samples.
    const sar::WaveFile parsed = sar::WaveFile::Read(incoming.bytes);
    for (std::size_t c = 0; c < back.channels.size() && back.loop; c++)
    {
        const std::vector<int16_t> later = LaterPass(parsed.channels[c].samples, *parsed.channels[c].adpcm,
                                                     back.loop_start, back.loop_end, built.ReadOptions().flavor);
        if (!std::equal(later.begin(), later.end(), back.channels[c].begin() + back.loop_start))
        {
            return "as DSP-ADPCM plays its loop's later passes otherwise";
        }
    }

    // The new wave archive keeps the other waves' bytes.
    const std::size_t place = wave.value_or(present.waves.size());
    const sar::WaveArchiveFile one =
        sar::WaveArchiveFile::Read(edit::WaveArchiveWith(built, item.item, wave, incoming.bytes));
    if (one.waves.size() != std::max(present.waves.size(), place + 1))
    {
        return "as DSP-ADPCM changes the number of waves";
    }
    for (std::size_t other = 0; other < one.waves.size(); other++)
    {
        if (one.waves[other] != (other == place ? incoming.bytes : present.waves[other]))
        {
            return "as DSP-ADPCM leaves wave " + std::to_string(other) + " with other bytes";
        }
    }

    if (ratio)
    {
        totals.adpcm_ratios.push_back(*ratio);
    }
    else
    {
        totals.adpcm_exact++;
    }
    totals.adpcm_moved += moved > 0 ? 1 : 0;
    totals.adpcm_samples += back.loop_end * back.channels.size();

    return {};
}

// What is wrong when wave `wave` of wave archive `item` comes back from its WAV file `wav` at half its rate, or from
// the WAV file without its loop. `pcm` holds the WAV file's samples, and `incoming` is the WAV file's wave as 16-bit
// PCM. At half the rate, the new wave has to hold the samples that audio::Resample gives each channel, with the loop's
// start at its place at the new rate. A wave at 8,000 Hz or less keeps its rate. Without its loop, the WAV file takes
// the present wave's loop that the Replace window offers. The new wave then has to hold the bytes that the WAV file
// with the loop gives.
std::string ResampledProblem(const sar::SoundArchive& built, const FileItem& item, uint32_t wave,
                             const edit::IncomingWave& incoming, std::span<const uint8_t> wav, const sar::Pcm& pcm,
                             Totals& totals)
{
    if (pcm.loop)
    {
        seqlib::Pcm unlooped;
        unlooped.channels = pcm.channels;
        const edit::IncomingWave offered =
            edit::ReadIncomingWave(seqlib::WavFile(unlooped, pcm.sample_rate), built, item.item, wave,
                                   sar::WaveEncoding::kPcm16, edit::WaveLoop{pcm.loop_start, pcm.loop_end});
        if (offered.bytes != incoming.bytes || !offered.loop_from_present)
        {
            return "doesn't come back from its WAV file without a loop, with the present wave's loop";
        }
        totals.loops_offered++;
    }
    if (pcm.sample_rate <= 8000)
    {
        totals.resampled_low++;
        return {};
    }

    const uint32_t rate = pcm.sample_rate / 2;
    const auto start = std::chrono::steady_clock::now();
    const edit::IncomingWave lower =
        edit::ReadIncomingWave(wav, built, item.item, wave, sar::WaveEncoding::kPcm16, std::nullopt, rate);
    totals.resampled_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const sar::Pcm back = sar::DecodeWave(lower.bytes, song::ItemOptions(built));
    const std::size_t loop_start = pcm.loop ? audio::ResampledPlace(pcm.loop_start, pcm.sample_rate, rate) : 0;
    if (lower.rate != rate || lower.wav_rate != pcm.sample_rate || lower.loop_start != loop_start ||
        back.sample_rate != rate || back.loop != pcm.loop || (pcm.loop && back.loop_start != loop_start) ||
        back.loop_end != lower.frames || back.channels.size() != pcm.channels.size())
    {
        return "doesn't come back from its WAV file at half its rate";
    }
    for (std::size_t c = 0; c < pcm.channels.size(); c++)
    {
        const std::vector<int16_t> expected =
            audio::Resample(pcm.channels[c], pcm.sample_rate, rate,
                            pcm.loop ? std::optional<std::size_t>(pcm.loop_start) : std::nullopt);
        if (back.channels[c] != expected)
        {
            return "comes back from its WAV file at half its rate with other samples";
        }
    }

    totals.resampled++;
    totals.resampled_samples += back.loop_end * back.channels.size();

    return {};
}

// What is wrong when each wave of wave archive `item` goes out as a WAV file and comes back in its place. The WAV file
// is the one that the browser's Export… writes, and the edit is Replace…'s. Each wave goes in alone first, as 16-bit
// PCM and as DSP-ADPCM. The wave archive with every wave from its WAV file as 16-bit PCM then takes the old one's
// place. `built` is the archive that `file` builds, with `groups`.
std::string WaveArchiveProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                               const FileItem& item, Totals& totals)
{
    const sar::WaveArchiveFile present = sar::WaveArchiveFile::Read(built.FileData(item.file_id));
    sar::WaveArchiveFile all = present;
    std::vector<sar::Pcm> decoded;
    for (uint32_t wave = 0; wave < present.waves.size(); wave++)
    {
        // The Replace window reads the WAV file and the present wave.
        const std::string label = "wave " + std::to_string(wave);
        decoded.push_back(song::DecodeItemWave(built, item.item, wave));
        const sar::Pcm& pcm = decoded.back();
        const std::vector<uint8_t> wav = song::WaveAsWav(built, item.item, wave);
        const edit::IncomingWave incoming =
            edit::ReadIncomingWave(wav, built, item.item, wave, sar::WaveEncoding::kPcm16);
        const edit::WaveChanges changes = edit::ChangesOfWave(built, item.item, wave);
        if (incoming.channels != pcm.channels.size() || incoming.rate != pcm.sample_rate || incoming.loop != pcm.loop ||
            incoming.loop_start != (pcm.loop ? pcm.loop_start : 0) || incoming.frames != pcm.loop_end ||
            incoming.after_loop != 0 || incoming.dropped != 0 || incoming.bits != 16 || incoming.floating)
        {
            return label + " doesn't come back from its WAV file";
        }
        if (changes.rate != pcm.sample_rate || changes.channels < pcm.channels.size() ||
            changes.size != present.waves[wave].size())
        {
            return label + " isn't the wave that the Replace window shows";
        }

        // The new wave archive holds the new wave in the wave's place, and the other waves' bytes.
        const sar::WaveArchiveFile one =
            sar::WaveArchiveFile::Read(edit::WaveArchiveWith(built, item.item, wave, incoming.bytes));
        if (one.waves.size() != present.waves.size())
        {
            return label + " changes the number of waves";
        }
        for (std::size_t other = 0; other < one.waves.size(); other++)
        {
            if (one.waves[other] != (other == wave ? incoming.bytes : present.waves[other]))
            {
                return label + " leaves wave " + std::to_string(other) + " with other bytes";
            }
        }

        // The new wave plays the old one's samples. As DSP-ADPCM, it plays them closely.
        if (!SamePcm(sar::DecodeWave(incoming.bytes, song::ItemOptions(built)), pcm))
        {
            return label + " plays other samples from its WAV file";
        }
        const std::string adpcm = DspAdpcmProblem(built, item, present, wave, wav, pcm, totals);
        if (!adpcm.empty())
        {
            return label + " " + adpcm;
        }
        const std::string resampled = ResampledProblem(built, item, wave, incoming, wav, pcm, totals);
        if (!resampled.empty())
        {
            return label + " " + resampled;
        }

        // The wave's file goes back in its place as it is. The wave archive then holds the same waves.
        const sar::Flavor flavor = built.ReadOptions().flavor;
        const song::Item wave_item{song::ItemKind::kWave, item.item, wave};
        const std::span<const uint8_t> wave_file = song::ItemFile(built, wave_item);
        const edit::IncomingWave same = edit::ReadIncomingWaveFile(wave_file, built, item.item, wave);
        const std::vector<uint8_t> again = edit::WaveArchiveWith(built, item.item, wave, same.bytes);
        if (!std::ranges::equal(same.bytes, wave_file) || same.converted ||
            sar::WaveArchiveFile::Read(again).waves != present.waves)
        {
            return label + " doesn't go back in as it is from its wave file";
        }
        totals.waves_rewritten += !std::ranges::equal(again, built.FileData(item.file_id)) ? 1 : 0;

        // The wave's file for each other console goes back in converted, with the wave's version and its samples.
        const uint32_t version = HeaderVersion(present.waves[wave], flavor);
        const sar::Options options = song::ItemOptions(built);
        for (const sar::Flavor console : song::OtherConsoles(flavor))
        {
            const std::vector<uint8_t> other = song::OtherConsoleFile(built, wave_item, console);
            const edit::IncomingWave converted = edit::ReadIncomingWaveFile(other, built, item.item, wave);
            if (!converted.converted || converted.version != version ||
                HeaderVersion(converted.bytes, flavor) != version ||
                !SamePcm(sar::DecodeWave(converted.bytes, options), sar::DecodeWave(present.waves[wave], options)))
            {
                return label + " doesn't come back from its wave file for the " + song::ConsoleName(console);
            }
            totals.waves_other_bytes += converted.bytes != present.waves[wave] ? 1 : 0;
        }

        totals.waves++;
        totals.waves_wide += changes.channels > 2 ? 1 : 0;
        totals.waves_loop_lost += changes.loop != incoming.loop ? 1 : 0;
        CountLaterPasses(present.waves[wave], pcm, built.ReadOptions().flavor, totals);
        all.waves[wave] = incoming.bytes;
    }

    // Every wave from its WAV file at once. The archive has to play the same samples.
    Archive after;
    const std::string problem = FileProblem(file, built, groups, item, all.Write(), after);
    if (!problem.empty())
    {
        return "with every wave from its WAV file " + problem;
    }
    for (uint32_t wave = 0; wave < present.waves.size(); wave++)
    {
        if (!SamePcm(song::DecodeItemWave(*after, item.item, wave), decoded[wave]))
        {
            return "with every wave from its WAV file plays other samples in wave " + std::to_string(wave);
        }
    }

    return {};
}

// Checks each wave of each wave archive with its WAV file. A wave archive that neither the archive nor its group files
// hold, and one whose bytes two file IDs name, stay out: Replace… refuses them.
void CheckWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                const std::string& name, Totals& totals)
{
    for (const FileItem& item : FileItemsOf(built))
    {
        if (item.kind != edit::FileKind::kWaveArchive)
        {
            continue;
        }
        if (Outside(built, item.file_id) || SharedFile(built, item.file_id))
        {
            totals.wave_archives_out++;
            continue;
        }

        totals.wave_archives_beside += Beside(built, item.file_id) ? 1 : 0;

        std::string problem;
        try
        {
            problem = WaveArchiveProblem(file, built, groups, item, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), NameOf(built, item).c_str(), problem.c_str());
            totals.wave_archives_failed++;
        }
        totals.wave_archives++;
    }
}

// The most sequence sounds that --new copies in an archive.
constexpr std::size_t kNewModels = 32;

// The sequence sounds that --new copies: up to kNewModels of the sounds with places, spread through the sound table.
std::vector<uint32_t> NewModels(std::span<const std::optional<edit::SoundPlace>> places)
{
    std::vector<uint32_t> candidates;
    for (std::size_t i = 0; i < places.size(); i++)
    {
        if (places[i])
        {
            candidates.push_back(static_cast<uint32_t>(i));
        }
    }

    const std::size_t count = std::min(candidates.size(), kNewModels);
    std::vector<uint32_t> models;
    for (std::size_t k = 0; k < count; k++)
    {
        models.push_back(candidates[k * candidates.size() / count]);
    }

    return models;
}

// A name that no item of the archive in `bytes` holds: HOKUTO_NEW_SOUND, with underscores after it while an item holds
// the name.
std::string FreeName(std::span<const uint8_t> bytes, sar::Flavor flavor)
{
    std::string name = "HOKUTO_NEW_SOUND";
    const std::optional<std::span<const uint8_t>> strings = sar::TablesOf(bytes, flavor).strings;
    while (strings && sar::FindItem(*strings, flavor, name))
    {
        name += "_";
    }

    return name;
}

// Whether two sounds hold the same values in the tables, but for their names, files, starts and tracks.
bool SameValues(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return a.type == b.type && a.volume == b.volume && a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve &&
           a.player_id == b.player_id && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id && a.banks == b.banks && a.channel_priority == b.channel_priority &&
           a.release_priority_fix == b.release_priority_fix;
}

// The names of a list of items.
template <typename Item>
std::vector<std::string> NamesOf(const std::vector<Item>& items)
{
    std::vector<std::string> names;
    for (const Item& item : items)
    {
        names.push_back(item.name);
    }

    return names;
}

// What is wrong with `added`, the version of `base` whose archive gains `name`, a copy of sound `model` that plays
// `source` from its start, or nothing. `built` is the archive that `base` builds. The new sound has to hold the model's
// values and play the model's notes, unless `untracked` says that the model leaves out tracks that its commands open.
// Every other item has to keep its values, and every file its bytes. The program has to find the new sound's sequence,
// and the new version has to save and open again in both forms.
std::string NewProblem(const format::File& base, const sar::SoundArchive& built, const format::File& added,
                       uint32_t model, const std::string& name, const Source& source, bool untracked)
{
    const std::vector<uint8_t> bytes = format::ExportArchive(added);
    const Archive after = song::LoadArchive(bytes, "after.bfsar", format::ExportGroupFiles(added));
    const auto sound = static_cast<uint32_t>(built.Sounds().size());
    if (after->Sounds().size() != sound + 1 || after->FileCount() != built.FileCount() + 1 ||
        added.sequences.size() != base.sequences.size() + 1)
    {
        return "doesn't give the archive one sound, one file and one sequence more";
    }

    // The new sound.
    const sar::SoundInfo& info = after->Sounds()[sound];
    if (info.name != name || info.file_id != built.FileCount() || !SameValues(info, built.Sounds()[model]))
    {
        return "gives the new sound other values than the model's";
    }
    if (info.allocate_track_flags != edit::TracksOpened(source.incoming.sequence, *source.start))
    {
        return "gives the new sound other tracks than its commands open";
    }
    if (!untracked && Played(*after, sound) != Played(built, model))
    {
        return "doesn't play the model's notes";
    }

    // The other items and files.
    for (uint32_t i = 0; i < sound; i++)
    {
        const sar::SoundInfo& old = built.Sounds()[i];
        const sar::SoundInfo& now = after->Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags || !SameValues(now, old))
        {
            return "changes " + old.name;
        }
    }
    if (Played(*after, model) != Played(built, model))
    {
        return "changes how the model plays";
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (!std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }
    if (NamesOf(after->Banks()) != NamesOf(built.Banks()) ||
        NamesOf(after->WaveArchives()) != NamesOf(built.WaveArchives()) ||
        NamesOf(after->Players()) != NamesOf(built.Players()) || NamesOf(after->Groups()) != NamesOf(built.Groups()))
    {
        return "changes the names of other items";
    }

    // The program's view of the new version.
    const std::vector<std::optional<edit::SoundPlace>> places = edit::SoundPlaces(
        *after, format::SequencesOfFiles(added, bytes, *after), format::StartFieldsOfSounds(added, bytes));
    if (!places[sound] || places[sound]->sequence != base.sequences.size())
    {
        return "leaves the new sound without its sequence";
    }
    if (!hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// Copies of the sequence sounds of NewModels in `file`, each with its file from the archive and for each other console.
// The second model's copy also goes in after the first model's. `built` is the archive that `file` builds, in `bytes`,
// with `edit_groups`. Up to kJoins copies join the groups that the Import window proposes.
void CheckNewSounds(const format::File& file, const sar::SoundArchive& built, std::span<const uint8_t> bytes,
                    std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
                    const std::string& name, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::vector<uint32_t> models = NewModels(places);
    std::optional<format::File> first;
    std::size_t joins = 0;
    bool new_group = false;
    for (std::size_t k = 0; k < models.size(); k++)
    {
        const uint32_t model = models[k];
        const sar::SoundInfo& info = built.Sounds()[model];
        std::string problem;
        try
        {
            const Source source = SourceOf(built.FileData(info.file_id), info.start_offset);
            if (!source.start)
            {
                totals.new_no_start++;
                continue;
            }
            const bool untracked = Untracked(source, info);
            totals.new_untracked += untracked ? 1 : 0;

            // The copy with the model's file. The Import window makes the same edit.
            const std::string new_name = FreeName(bytes, flavor);
            const auto start = std::chrono::steady_clock::now();
            const format::File added =
                edit::AddSequenceSound(file, model, new_name, source.incoming.sequence, *source.start, {});
            const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
            totals.new_slowest = std::max(totals.new_slowest, took.count());
            problem = NewProblem(file, built, added, model, new_name, source, untracked);

            // The same commands from the file for each other console give the same archive. A file that the 3DS can't
            // hold counts apart.
            for (const sar::Flavor console : song::OtherConsoles(flavor))
            {
                if (!problem.empty())
                {
                    break;
                }

                std::vector<uint8_t> other_file;
                try
                {
                    other_file = song::OtherConsoleSequenceFile(built, model, console).file;
                }
                catch (const std::runtime_error&)
                {
                    if (console != sar::Flavor::kCtr)
                    {
                        throw;
                    }
                    totals.new_ctr_refused++;
                    continue;
                }
                const Source other = SourceOf(other_file, info.start_offset, song::Console{.flavor = flavor});
                if (other.start && other.incoming.sequence != source.incoming.sequence &&
                    song::SameButBiquads(other.incoming.sequence, source.incoming.sequence))
                {
                    totals.new_other_biquads++;
                }
                else if (!other.start || other.incoming.sequence != source.incoming.sequence)
                {
                    totals.new_other_unreached++;
                }
                else if (format::ExportArchive(edit::AddSequenceSound(file, model, new_name, other.incoming.sequence,
                                                                      *other.start, {})) !=
                         format::ExportArchive(added))
                {
                    problem = "gives another archive from the file for the " + song::ConsoleName(console);
                }
                else
                {
                    totals.new_crossed++;
                }
            }

            // The copy joins the groups that load the model. The first copy also joins a new group.
            const bool joining = !edit_groups.empty() && joins < kJoins;
            if (problem.empty() && (joining || !new_group))
            {
                const uint32_t id = sar::ItemId(sar::kSoundItem, static_cast<uint32_t>(built.Sounds().size()));
                const std::vector<uint32_t> files_before =
                    edit::FilesOfItem(built, sar::ItemId(sar::kSoundItem, model));
                const Choice chosen =
                    joining ? ChosenGroups(file, built, edit_groups, files_before, {}, joins == 0, totals) : Choice();
                if (!chosen.groups.empty())
                {
                    joins++;
                    problem = JoinCheck(added, chosen, std::span(&id, 1), files_before, totals);
                }
                if (problem.empty() && !new_group)
                {
                    new_group = true;
                    problem = NewGroupCheck(added, built, std::span(&id, 1), files_before, totals);
                }
            }

            // The second model's copy after the first model's.
            if (problem.empty() && k == 0)
            {
                first = added;
            }
            else if (problem.empty() && k == 1 && first)
            {
                const std::vector<uint8_t> first_bytes = format::ExportArchive(*first);
                const Archive first_built =
                    song::LoadArchive(first_bytes, "first.bfsar", format::ExportGroupFiles(*first));
                const std::string second_name = FreeName(first_bytes, flavor);
                const format::File twice =
                    edit::AddSequenceSound(*first, model, second_name, source.incoming.sequence, *source.start, {});
                problem = NewProblem(*first, *first_built, twice, model, second_name, source, untracked);
                totals.new_pairs++;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.new_failed++;
        }
        totals.new_sounds++;
    }
}

// The most sequence sounds that --banks copies in an archive, and the seconds of each render.
constexpr std::size_t kBankModels = 8;
constexpr double kBankSeconds = 10.0;

// The bits of an item ID that give the item's place in its list.
constexpr uint32_t kIndexBits = 0x00FFFFFF;

// A name that no item of `tables` holds: `stem`, with underscores after it while an item holds the name.
std::string FreeTableName(const format::NativeTables& tables, std::string stem)
{
    const auto taken = [&tables](const std::string& name)
    {
        const auto named = [&name](const auto& item)
        {
            return item.name == name;
        };
        const auto same = [&name](const std::string& other)
        {
            return other == name;
        };
        return std::ranges::any_of(tables.sounds, named) || std::ranges::any_of(tables.banks, same) ||
               std::ranges::any_of(tables.wave_archives, same) || std::ranges::any_of(tables.players, named) ||
               std::ranges::any_of(tables.groups, same);
    };
    while (taken(stem))
    {
        stem += "_";
    }

    return stem;
}

// The samples of sound `sound` of `archive` on ntdlib's model of the console, for up to kBankSeconds.
std::vector<int16_t> Rendered(const Archive& archive, uint32_t sound)
{
    const auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, sound));
    song::SequenceModel model(archive, sequence);
    std::vector<int16_t> samples;
    const auto frames = static_cast<uint64_t>(kBankSeconds * model.SampleRate()) / model.FrameSamples();
    for (bool running = model.SeekFrame(0); running; running = model.Frames() < frames && model.RunFrame())
    {
        const std::span<const int16_t> output = model.Output();
        samples.insert(samples.end(), output.begin(), output.end());
    }

    return samples;
}

// The new items of one model: the new banks' item IDs by the old banks' item IDs, the new wave archives' places by the
// old wave archives' item IDs, and the new sound's place.
struct NewItems
{
    std::map<uint32_t, uint32_t> banks;
    std::map<uint32_t, uint32_t> wave_archives;
    uint32_t sound = 0;
};

// The file of bank `bank` of `archive`, by its item ID. Empty for a bank past the archive's list or without its file.
std::span<const uint8_t> BankFile(const sar::SoundArchive& archive, uint32_t bank)
{
    const uint32_t index = bank & kIndexBits;
    return index < archive.Banks().size() ? archive.FileData(archive.Banks()[index].file_id)
                                          : std::span<const uint8_t>{};
}

// The banks in the slots of `info`, a sound of `archive`, each once. Nothing when a bank or a wave archive that its
// waves name lacks its file.
std::optional<std::vector<uint32_t>> ModelBanks(const sar::SoundArchive& archive, const sar::SoundInfo& info)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<uint32_t> banks;
    for (const uint32_t bank : info.banks)
    {
        if (bank == sar::kNoItem || std::ranges::find(banks, bank) != banks.end())
        {
            continue;
        }

        const std::span<const uint8_t> bytes = BankFile(archive, bank);
        if (bytes.empty())
        {
            return std::nullopt;
        }
        for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
        {
            const uint32_t index = wave_archive.id & kIndexBits;
            if (index >= archive.WaveArchives().size() ||
                archive.FileData(archive.WaveArchives()[index].file_id).empty())
            {
                return std::nullopt;
            }
        }
        banks.push_back(bank);
    }

    return banks;
}

// The file after its archive gains copies of `banks`, banks of `built`, through the edits of the Import windows. Copies
// of the wave archives that their waves name come first. A copy of a wave archive with a name takes a new name. The new
// banks' waves lie in the new wave archives. Then the archive gains a copy of sound `model` that plays the new banks
// from `source`. Keeps the longest time that an edit took in `slowest`.
std::pair<format::File, NewItems> WithCopies(const format::File& file, const sar::SoundArchive& built, uint32_t model,
                                             std::span<const uint32_t> banks, const Source& source, double& slowest)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;

    const auto timed = [&slowest](std::chrono::steady_clock::time_point start)
    {
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
        slowest = std::max(slowest, took.count());
    };

    // Each bank after the wave archives that it requires.
    format::File added = file;
    NewItems items;
    for (const uint32_t bank : banks)
    {
        const std::span<const uint8_t> bytes = BankFile(built, bank);
        std::map<uint32_t, uint32_t> map;
        for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
        {
            const uint32_t index = wave_archive.id & kIndexBits;
            if (!items.wave_archives.contains(wave_archive.id))
            {
                const format::NativeTables& tables = *added.native->tables;
                const std::string wave_archive_name =
                    tables.wave_archives[index].empty() ? "" : FreeTableName(tables, "HOKUTO_NEW_WARC");
                items.wave_archives[wave_archive.id] = static_cast<uint32_t>(tables.wave_archives.size());
                const auto start = std::chrono::steady_clock::now();
                added =
                    edit::AddWaveArchive(added, wave_archive_name, built.FileData(built.WaveArchives()[index].file_id));
                timed(start);
            }
            map[wave_archive.id] = items.wave_archives[wave_archive.id];
        }

        items.banks[bank] = sar::ItemId(sar::kBankItem, static_cast<uint32_t>(added.native->tables->banks.size()));
        const std::string bank_name = FreeTableName(*added.native->tables, "HOKUTO_NEW_BANK");
        const auto start = std::chrono::steady_clock::now();
        added = edit::AddBank(added, bank_name, bytes, map);
        timed(start);
    }

    // The copy of the model, with the new banks in the model's slots.
    std::vector<uint32_t> slots = built.Sounds()[model].banks;
    for (uint32_t& slot : slots)
    {
        slot = slot == sar::kNoItem ? slot : items.banks.at(slot);
    }
    items.sound = static_cast<uint32_t>(added.native->tables->sounds.size());
    const std::string sound_name = FreeTableName(*added.native->tables, "HOKUTO_NEW_SOUND");
    const auto start = std::chrono::steady_clock::now();
    added = edit::AddSequenceSound(added, model, sound_name, source.incoming.sequence, *source.start, {}, slots);
    timed(start);

    return {std::move(added), std::move(items)};
}

// What is wrong with `added`, the version of `file` whose archive gains `items`: copies of the banks in sound `model`'s
// slots and of their wave archives, and a copy of the model that plays the new banks. `built` is the archive that
// `file` builds. Returns nothing when nothing is wrong. Counts a new sound that stays silent.
std::string BankProblem(const format::File& added, const Archive& built, uint32_t model, const NewItems& items,
                        Totals& totals)
{
    const sar::Flavor flavor = built->ReadOptions().flavor;
    hokuto::tools::CheckSaved(added, true);
    const Archive after =
        song::LoadArchive(format::ExportArchive(added), "after.bfsar", format::ExportGroupFiles(added));
    const std::size_t files = items.banks.size() + items.wave_archives.size() + 1;
    if (after->Sounds().size() != built->Sounds().size() + 1 ||
        after->Banks().size() != built->Banks().size() + items.banks.size() ||
        after->WaveArchives().size() != built->WaveArchives().size() + items.wave_archives.size() ||
        after->FileCount() != built->FileCount() + files)
    {
        return "doesn't give the archive the new items";
    }

    // The other items and files.
    const auto same_item = [](const auto& a, const auto& b)
    {
        return a.name == b.name && a.file_id == b.file_id;
    };
    for (uint32_t i = 0; i < built->Sounds().size(); i++)
    {
        const sar::SoundInfo& old = built->Sounds()[i];
        const sar::SoundInfo& now = after->Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags || !SameValues(now, old))
        {
            return "changes " + old.name;
        }
    }
    if (!std::ranges::equal(built->Banks(), std::span(after->Banks()).first(built->Banks().size()), same_item) ||
        !std::ranges::equal(built->WaveArchives(), std::span(after->WaveArchives()).first(built->WaveArchives().size()),
                            same_item))
    {
        return "changes the archive's banks or wave archives";
    }
    for (uint32_t id = 0; id < built->FileCount(); id++)
    {
        if (!std::ranges::equal(built->FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }

    // The new wave archives hold the old files' bytes, and the new banks the old banks' bytes but for the wave archives
    // that their waves name.
    std::map<uint32_t, uint32_t> ids;
    for (const auto& [old, place] : items.wave_archives)
    {
        ids[old] = sar::ItemId(sar::kWaveArchiveItem, place);
        const std::span<const uint8_t> before = built->FileData(built->WaveArchives()[old & kIndexBits].file_id);
        if (!std::ranges::equal(after->FileData(after->WaveArchives()[place].file_id), before))
        {
            return "gives a new wave archive other bytes";
        }
    }
    for (const auto& [old, now] : items.banks)
    {
        const std::vector<uint8_t> expected = sar::WithWaveArchives(BankFile(*built, old), flavor, ids);
        if (!std::ranges::equal(BankFile(*after, now), expected))
        {
            return "gives a new bank other bytes";
        }
    }

    // The new sound holds the model's values with the new banks, and plays the model's samples.
    sar::SoundInfo expected = built->Sounds()[model];
    for (uint32_t& bank : expected.banks)
    {
        bank = bank == sar::kNoItem ? bank : items.banks.at(bank);
    }
    if (!SameValues(after->Sounds()[items.sound], expected))
    {
        return "gives the new sound other values than the model's with the new banks";
    }
    const std::vector<int16_t> samples = Rendered(after, items.sound);
    if (samples != Rendered(built, model))
    {
        return "plays other samples than the model";
    }
    totals.bank_silent += std::ranges::all_of(samples, [](int16_t sample) { return sample == 0; }) ? 1 : 0;

    return {};
}

// Copies of the banks of up to kBankModels sequence sounds of `file`, with copies of their wave archives, and a copy of
// each sound that plays the new banks. `built` is the archive that `file` builds. The models are those of --new, taken
// in an order that spreads them through the sound table.
void CheckNewBanks(const format::File& file, const Archive& built,
                   std::span<const std::optional<edit::SoundPlace>> places, const std::string& name, Totals& totals)
{
    const std::vector<uint32_t> models = NewModels(places);
    const std::size_t stride = std::max<std::size_t>(1, models.size() / kBankModels);
    std::vector<uint32_t> order;
    for (std::size_t first = 0; first < stride; first++)
    {
        for (std::size_t i = first; i < models.size(); i += stride)
        {
            order.push_back(models[i]);
        }
    }

    for (std::size_t k = 0; k < order.size() && totals.bank_checked < kBankModels; k++)
    {
        const uint32_t model = order[k];
        const sar::SoundInfo& info = built->Sounds()[model];
        std::string problem;
        try
        {
            const Source source = SourceOf(built->FileData(info.file_id), info.start_offset);
            const std::optional<std::vector<uint32_t>> banks = ModelBanks(*built, info);
            if (!source.start || Untracked(source, info))
            {
                totals.bank_skipped++;
                continue;
            }
            if (!banks)
            {
                totals.bank_missing++;
                continue;
            }
            if (banks->empty())
            {
                totals.bankless++;
                continue;
            }

            totals.bank_checked++;
            const auto [added, items] = WithCopies(file, *built, model, *banks, source, totals.bank_slowest);
            problem = BankProblem(added, built, model, items, totals);
            totals.new_banks += items.banks.size();
            totals.new_wave_archives += items.wave_archives.size();
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.bank_failed++;
        }
    }
}

// The most wave sound files that --wave-sounds copies from a donor.
constexpr std::size_t kWaveSoundFiles = 8;

// The wave sound files of `archive` whose wave archives the reader reads, each once, in the order of the sounds that
// play them: each file's ID, and the first sound that plays it. Counts the others in `missing`.
std::vector<std::pair<uint32_t, uint32_t>> WaveSoundFilesOf(const sar::SoundArchive& archive, std::size_t& missing)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<std::pair<uint32_t, uint32_t>> files;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const sar::SoundInfo& sound = archive.Sounds()[i];
        if (sound.type != sar::SoundType::kWaveSound || !seen.insert(sound.file_id).second)
        {
            continue;
        }

        // The file, and the files of the wave archives that its waves name.
        const std::span<const uint8_t> bytes = archive.FileData(sound.file_id);
        bool readable = !bytes.empty();
        if (readable)
        {
            for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
            {
                const uint32_t index = wave_archive.id & kIndexBits;
                readable = readable && index < archive.WaveArchives().size() &&
                           !archive.FileData(archive.WaveArchives()[index].file_id).empty();
            }
        }
        if (readable)
        {
            files.emplace_back(sound.file_id, i);
        }
        else
        {
            missing++;
        }
    }

    return files;
}

// A stem of names, from `stem`, such that no item of `tables` holds the stem or any name that edit::WaveSoundNames
// makes from it for `count` sounds.
std::string FreeStem(const format::NativeTables& tables, std::string stem, std::size_t count)
{
    const auto unused = [&tables](const std::string& name)
    {
        return FreeTableName(tables, name) == name;
    };
    while (!unused(stem) || !std::ranges::all_of(edit::WaveSoundNames(stem, count), unused))
    {
        stem += "_";
    }

    return stem;
}

// The new items of a wave sound file: the new wave archives' places by the donor's wave archives' item IDs, and the new
// sounds' names and the place of the first. The bytes of each new wave archive and of the file as the Import window
// reads them, and the files among them that went in written again with the archive's versions.
struct NewWaveSounds
{
    std::map<uint32_t, uint32_t> wave_archives;
    std::vector<std::string> names;
    uint32_t first = 0;
    std::map<uint32_t, std::vector<uint8_t>> wave_archive_bytes;
    std::vector<uint8_t> file_bytes;
    std::size_t written = 0;
};

// The file after its archive gains copies of the wave archives that the waves of file `donor_file` of `donor` name,
// then that file as new wave sounds that copy sound `model` of the archive, through the reads and the edits of the
// Import windows. `built` is the archive that `file` builds. A copy of a wave archive takes a new name where the
// archive's wave archives have names. Keeps the longest time that an edit took in `slowest`.
std::pair<format::File, NewWaveSounds> WithWaveSounds(const format::File& file, const sar::SoundArchive& built,
                                                      const sar::SoundArchive& donor, uint32_t donor_file,
                                                      uint32_t model, double& slowest)
{
    const sar::Flavor flavor = donor.ReadOptions().flavor;

    const auto timed = [&slowest](std::chrono::steady_clock::time_point start)
    {
        const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
        slowest = std::max(slowest, took.count());
    };

    // The wave archives that the file's waves name.
    const std::span<const uint8_t> bytes = donor.FileData(donor_file);
    format::File added = file;
    NewWaveSounds items;
    std::map<uint32_t, uint32_t> map;
    for (const edit::NamedWaveArchive& wave_archive : edit::WaveArchivesNamed(bytes, flavor))
    {
        const format::NativeTables& tables = *added.native->tables;
        const bool named =
            std::ranges::any_of(tables.wave_archives, [](const std::string& each) { return !each.empty(); });
        const std::string wave_archive_name = named ? FreeTableName(tables, "HOKUTO_NEW_WARC") : "";
        items.wave_archives[wave_archive.id] = static_cast<uint32_t>(tables.wave_archives.size());
        const sar::WaveArchiveInfo& old = donor.WaveArchives()[wave_archive.id & kIndexBits];
        const edit::IncomingFile incoming =
            edit::ReadIncomingFile(donor.FileData(old.file_id), edit::FileKind::kWaveArchive, flavor,
                                   edit::IncomingVersions(built, edit::FileKind::kWaveArchive));
        items.written += incoming.converted ? 1 : 0;
        const auto start = std::chrono::steady_clock::now();
        added = edit::AddWaveArchive(added, wave_archive_name, incoming.bytes);
        timed(start);
        items.wave_archive_bytes[wave_archive.id] = incoming.bytes;
        map[wave_archive.id] = items.wave_archives[wave_archive.id];
    }

    // The file as new wave sounds.
    const edit::IncomingFile incoming = edit::ReadIncomingFile(
        bytes, edit::FileKind::kWaveSoundFile, flavor, edit::IncomingVersions(built, edit::FileKind::kWaveSoundFile));
    items.written += incoming.converted ? 1 : 0;
    items.file_bytes = incoming.bytes;
    const std::size_t count = edit::WaveSoundCount(items.file_bytes, flavor);
    const std::string stem = FreeStem(*added.native->tables, "HOKUTO_NEW_WSD", count);
    items.names = edit::WaveSoundNames(stem, count);
    items.first = static_cast<uint32_t>(added.native->tables->sounds.size());
    const auto start = std::chrono::steady_clock::now();
    added = edit::AddWaveSounds(added, model, stem, items.file_bytes, map);
    timed(start);

    return {std::move(added), std::move(items)};
}

// What is wrong with `added`, the version of `file` whose archive gains `items`: copies of the wave archives that the
// waves of file `donor_file` of `donor` name, and the file's wave sounds as copies of sound `model`. `built` is the
// archive that `file` builds. Returns nothing when nothing is wrong. Counts the new sounds whose samples are compared.
std::string WaveSoundProblem(const format::File& added, const sar::SoundArchive& built, const sar::SoundArchive& donor,
                             uint32_t donor_file, uint32_t model, const NewWaveSounds& items, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const Archive after =
        song::LoadArchive(format::ExportArchive(added), "after.bfsar", format::ExportGroupFiles(added));
    const std::size_t count = items.names.size();
    if (after->Sounds().size() != built.Sounds().size() + count ||
        after->WaveArchives().size() != built.WaveArchives().size() + items.wave_archives.size() ||
        after->FileCount() != built.FileCount() + items.wave_archives.size() + 1)
    {
        return "doesn't give the archive the new items";
    }

    // The other items and files.
    const auto same_item = [](const auto& a, const auto& b)
    {
        return a.name == b.name && a.file_id == b.file_id;
    };
    for (uint32_t i = 0; i < built.Sounds().size(); i++)
    {
        const sar::SoundInfo& old = built.Sounds()[i];
        const sar::SoundInfo& now = after->Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags || now.wave_sound_index != old.wave_sound_index ||
            !SameValues(now, old))
        {
            return "changes " + old.name;
        }
    }
    if (!std::ranges::equal(built.Banks(), after->Banks(), same_item) ||
        !std::ranges::equal(built.WaveArchives(), std::span(after->WaveArchives()).first(built.WaveArchives().size()),
                            same_item))
    {
        return "changes the archive's banks or wave archives";
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (!std::ranges::equal(built.FileData(id), after->FileData(id)))
        {
            return "changes file " + std::to_string(id);
        }
    }

    // The new wave archives hold the bytes that the Import window read, and the new file those bytes but for the wave
    // archives that its waves name.
    std::map<uint32_t, uint32_t> ids;
    for (const auto& [old, place] : items.wave_archives)
    {
        ids[old] = sar::ItemId(sar::kWaveArchiveItem, place);
        if (!std::ranges::equal(after->FileData(after->WaveArchives()[place].file_id),
                                items.wave_archive_bytes.at(old)))
        {
            return "gives a new wave archive other bytes";
        }
    }
    const auto new_file = static_cast<uint32_t>(after->FileCount() - 1);
    if (!std::ranges::equal(after->FileData(new_file), sar::WithWaveArchives(items.file_bytes, flavor, ids)))
    {
        return "gives the new file other bytes";
    }

    // Each new sound holds the model's values and plays the donor's samples. A wave sound has no banks. A copy of a
    // sequence sound keeps the model's channel priority and release priority fix in its details.
    sar::SoundInfo expected = built.Sounds()[model];
    if (expected.type == sar::SoundType::kSequence)
    {
        expected.type = sar::SoundType::kWaveSound;
        expected.banks = sar::SoundInfo().banks;
    }
    for (uint32_t k = 0; k < count; k++)
    {
        const uint32_t sound = items.first + k;
        const sar::SoundInfo& info = after->Sounds()[sound];
        if (info.name != items.names[k] || info.file_id != new_file || info.wave_sound_index != k ||
            !SameValues(info, expected))
        {
            return "gives " + items.names[k] + " other values than the model's";
        }

        // The donor's first sound that plays the wave sound, decoded on each side.
        const auto plays = [donor_file, k](const sar::SoundInfo& other)
        {
            return other.type == sar::SoundType::kWaveSound && other.file_id == donor_file &&
                   other.wave_sound_index == k;
        };
        const auto player = std::ranges::find_if(donor.Sounds(), plays);
        if (player == donor.Sounds().end())
        {
            totals.wsd_unplayed++;
            continue;
        }

        const auto decoded = [](const sar::SoundArchive& archive, uint32_t index) -> std::optional<sar::Pcm>
        {
            try
            {
                return song::DecodeWaveSound(archive, index);
            }
            catch (const std::runtime_error&)
            {
                return std::nullopt;
            }
        };
        const std::optional<sar::Pcm> theirs = decoded(donor, static_cast<uint32_t>(player - donor.Sounds().begin()));
        const std::optional<sar::Pcm> ours = decoded(*after, sound);
        if (theirs.has_value() != ours.has_value() || (theirs && !SamePcm(*theirs, *ours)))
        {
            return items.names[k] + " plays other samples than " + player->name;
        }
        totals.wsd_compared += theirs ? 1 : 0;
        totals.wsd_undecoded += theirs ? 0 : 1;
    }

    // The new version saves and opens again.
    if (!hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// The wave sound files of `donor`, up to kWaveSoundFiles, as new wave sounds of `file`. `built` is the archive that
// `file` builds, with `edit_groups`, and `self` says that the donor is that archive. Each file goes in once with a wave
// sound as the model, and once with the archive's first sequence sound. Up to kJoins times, the new sounds join the
// groups that the Import window proposes.
void CheckNewWaveSounds(const format::File& file, const sar::SoundArchive& built,
                        std::span<const edit::Group> edit_groups, const sar::SoundArchive& donor, bool self,
                        const std::string& name, Totals& totals)
{
    std::size_t joins = 0;
    bool new_group = false;

    // The models: the sound that plays the file in the archive itself, or else the archive's first wave sound, then the
    // archive's first sequence sound.
    const auto first_of = [&built](sar::SoundType type) -> std::optional<uint32_t>
    {
        const auto found = std::ranges::find(built.Sounds(), type, &sar::SoundInfo::type);
        if (found == built.Sounds().end())
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(found - built.Sounds().begin());
    };
    const std::optional<uint32_t> wave_sound = first_of(sar::SoundType::kWaveSound);
    const std::optional<uint32_t> sequence = first_of(sar::SoundType::kSequence);
    const std::vector<std::pair<uint32_t, uint32_t>> files = WaveSoundFilesOf(donor, totals.wsd_missing);
    for (std::size_t f = 0; f < files.size() && f < kWaveSoundFiles; f++)
    {
        const auto [donor_file, player] = files[f];
        std::vector<uint32_t> models;
        if (self || wave_sound)
        {
            models.push_back(self ? player : *wave_sound);
        }
        if (sequence)
        {
            models.push_back(*sequence);
        }

        for (const uint32_t model : models)
        {
            std::string problem;
            try
            {
                const auto [added, items] = WithWaveSounds(file, built, donor, donor_file, model, totals.wsd_slowest);
                problem = WaveSoundProblem(added, built, donor, donor_file, model, items, totals);
                totals.wsd_sounds += items.names.size();
                totals.wsd_written += items.written;
                const bool copied_sequence = built.Sounds()[model].type == sar::SoundType::kSequence;
                totals.wsd_from_sequences += copied_sequence ? items.names.size() : 0;
                totals.wsd_wave_archives += items.wave_archives.size();

                // The new sounds join the groups that load the model. The first import's sounds also join a new group.
                const bool joining = !edit_groups.empty() && joins < kJoins;
                if (problem.empty() && (joining || !new_group))
                {
                    std::vector<uint32_t> sounds;
                    for (std::size_t k = 0; k < items.names.size(); k++)
                    {
                        sounds.push_back(sar::ItemId(sar::kSoundItem, static_cast<uint32_t>(items.first + k)));
                    }
                    const std::vector<uint32_t> files_before =
                        edit::FilesOfItem(built, sar::ItemId(sar::kSoundItem, model));
                    const Choice chosen =
                        joining ? ChosenGroups(file, built, edit_groups, files_before, {}, joins == 0, totals)
                                : Choice();
                    if (!chosen.groups.empty())
                    {
                        joins++;
                        problem = JoinCheck(added, chosen, sounds, files_before, totals);
                    }
                    if (problem.empty() && !new_group)
                    {
                        new_group = true;
                        problem = NewGroupCheck(added, built, sounds, files_before, totals);
                    }
                }
            }
            catch (const std::exception& error)
            {
                problem = std::string("fails: ") + error.what();
            }

            if (!problem.empty())
            {
                std::printf("%s: file %u with %s as the model %s\n", name.c_str(), donor_file,
                            built.Sounds()[model].name.c_str(), problem.c_str());
                totals.wsd_failed++;
            }
            totals.wsd_files++;
        }
    }
}

// The version that a new wave of wave archive `wave_archive` of `archive` takes where Hokuto makes or converts it: the
// version in the header of the wave archive's first wave, or else of the first wave of the archive's first wave archive
// with waves that ntdlib reads, or else the version of Export…'s waves.
uint32_t NewWaveVersion(const sar::SoundArchive& archive, uint32_t wave_archive)
{
    const sar::Flavor flavor = archive.ReadOptions().flavor;
    std::vector<uint32_t> order = {wave_archive};
    for (uint32_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        order.push_back(i);
    }
    for (const uint32_t i : order)
    {
        try
        {
            const std::span<const uint8_t> file = archive.FileData(archive.WaveArchives()[i].file_id);
            const std::vector<std::span<const uint8_t>> waves =
                sar::WaveArchive::Parse(file, archive.ReadOptions()).waves;
            if (!waves.empty() && waves[0].size() >= 0x14)
            {
                return HeaderVersion(waves[0], flavor);
            }
        }
        catch (const std::exception&)
        {
            // The next wave archive gives the version.
        }
    }

    return song::ExportVersions(song::ItemKind::kWaveArchive, flavor).waves;
}

// The first wave of the archive's first wave archive with waves, or nothing.
std::optional<song::Item> FirstWave(const sar::SoundArchive& archive)
{
    for (uint32_t i = 0; i < archive.WaveArchives().size(); i++)
    {
        try
        {
            if (song::WaveCountOf(archive, i) > 0)
            {
                return song::Item{song::ItemKind::kWave, i, 0};
            }
        }
        catch (const std::exception&)
        {
            // Another wave archive gives the wave.
        }
    }

    return std::nullopt;
}

// What is wrong when `incoming`, a wave that the Import window read, goes in after the waves of wave archive `item`, or
// nothing. `present` is the wave archive's file. The new wave has to decode to `pcm`.
std::string AppendProblem(const sar::SoundArchive& built, const FileItem& item, const sar::WaveArchiveFile& present,
                          const edit::IncomingWave& incoming, const sar::Pcm& pcm)
{
    const sar::WaveArchiveFile now =
        sar::WaveArchiveFile::Read(edit::WaveArchiveWith(built, item.item, std::nullopt, incoming.bytes));
    if (now.waves.size() != present.waves.size() + 1 || now.waves.back() != incoming.bytes)
    {
        return "doesn't put the new wave after the others";
    }
    for (std::size_t i = 0; i < present.waves.size(); i++)
    {
        if (now.waves[i] != present.waves[i])
        {
            return "leaves wave " + std::to_string(i) + " with other bytes";
        }
    }
    if (!SamePcm(sar::DecodeWave(incoming.bytes, song::ItemOptions(built)), pcm))
    {
        return "plays other samples";
    }

    return {};
}

// What is wrong when a short WAV file goes in as the first wave of wave archive `item`, in an archive without waves, or
// nothing. The new wave has to take the version of Export…'s waves and decode to the WAV file's samples. The edit then
// goes through the file as in NewWavesProblem. `built` is the archive that `file` builds, with `groups`.
std::string WavelessProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                            const FileItem& item, Totals& totals)
{
    // The WAV file goes in as 16-bit PCM.
    const sar::Flavor flavor = built.ReadOptions().flavor;
    seqlib::Pcm pcm;
    pcm.channels = {{0, 1000, -1000, 2000, -2000, 3000, -3000, 4000}};
    const std::vector<uint8_t> wav = seqlib::WavFile(pcm, 32000);
    const edit::IncomingWave incoming =
        edit::ReadIncomingWave(wav, built, item.item, std::nullopt, sar::WaveEncoding::kPcm16);
    const uint32_t version = song::ExportVersions(song::ItemKind::kWaveArchive, flavor).waves;
    if (incoming.version != version || HeaderVersion(incoming.bytes, flavor) != version)
    {
        return "doesn't take the version of Export…'s waves from a WAV file";
    }

    // The Import window's edit.
    edit::IncomingFile waves;
    waves.kind = edit::FileKind::kWaveArchive;
    waves.bytes = edit::WaveArchiveWith(built, item.item, std::nullopt, incoming.bytes);
    edit::ChangesOf(file, built, item.item, waves);
    Archive after;
    const std::string problem =
        ReplacedProblem(built, groups, item, waves.bytes, edit::ReplaceFile(file, item.file_id, waves.bytes), after);
    if (!problem.empty())
    {
        return "with a new wave " + problem;
    }
    const sar::Pcm decoded = song::DecodeItemWave(*after, item.item, 0);
    if (song::WaveCountOf(*after, item.item) != 1 || decoded.sample_rate != 32000 || decoded.loop ||
        decoded.channels != pcm.channels)
    {
        return "with a new wave plays other samples there";
    }
    totals.nw_waveless++;

    return {};
}

// What is wrong when the waves of wave archive `item` go in again as new waves after its waves, or nothing. Each wave
// goes in from its wave file, from its wave file for each other console and from its WAV file as 16-bit PCM, through
// the reads of the Import window. The first also goes in from its WAV file as DSP-ADPCM (DspAdpcmProblem). A wave
// archive without waves takes the archive's first wave instead, or a WAV file in an archive without waves
// (WavelessProblem). The new wave archive has to hold the old waves' bytes and the new wave after them. The new wave
// has to decode to the samples of the wave that it came from. A wave that Hokuto makes or converts has to take the
// version of the wave archive's waves (NewWaveVersion). The first wave's file then goes in through the Import window's
// edit. The exported archive has to hold the new wave archive in every copy, and every other file has to keep its bytes
// (ReplacedProblem). Until `paired` is set, the first wave's WAV file also goes in as a second new wave after the
// first, and the file has to save and open again in both forms. `built` is the archive that `file` builds, with
// `groups`.
std::string NewWavesProblem(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                            const FileItem& item, bool& paired, Totals& totals)
{
    // The wave archive, and the waves that go in.
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const sar::Options options = song::ItemOptions(built);
    const sar::WaveArchiveFile present = sar::WaveArchiveFile::Read(built.FileData(item.file_id));
    const uint32_t version = NewWaveVersion(built, item.item);
    std::vector<song::Item> sources;
    for (uint32_t wave = 0; wave < present.waves.size(); wave++)
    {
        sources.push_back({song::ItemKind::kWave, item.item, wave});
    }
    if (sources.empty())
    {
        const std::optional<song::Item> first = FirstWave(built);
        if (!first)
        {
            return WavelessProblem(file, built, groups, item, totals);
        }
        sources.push_back(*first);
        totals.nw_empty++;
    }

    std::vector<sar::Pcm> pcms;
    for (std::size_t k = 0; k < sources.size(); k++)
    {
        // The wave's file as the browser's Export… writes it.
        const song::Item& source = sources[k];
        const std::string label =
            source.index == item.item ? "wave " + std::to_string(source.wave) : song::ItemName(built, source);
        const std::span<const uint8_t> wave_file = song::ItemFile(built, source);
        pcms.push_back(song::DecodeItemWave(built, source.index, source.wave));
        const sar::Pcm& pcm = pcms.back();
        const sar::Pcm decoded = sar::DecodeWave(wave_file, options);

        // The wave file goes in as it is, or with the version of the wave archive's waves when it holds a later one.
        const edit::IncomingWave same = edit::ReadIncomingWaveFile(wave_file, built, item.item);
        const bool later = HeaderVersion(wave_file, flavor) > version;
        if (later ? !same.converted || same.converted->flavor != flavor || same.version != version ||
                        HeaderVersion(same.bytes, flavor) != version
                  : !std::ranges::equal(same.bytes, wave_file) || same.converted ||
                        same.version != HeaderVersion(wave_file, flavor))
        {
            return label + " doesn't go in as it is from its wave file";
        }
        totals.nw_written += later ? 1 : 0;
        if (same.rate != pcm.sample_rate || same.loop != pcm.loop || same.frames != pcm.loop_end ||
            same.loop_start != (pcm.loop ? pcm.loop_start : 0) ||
            same.channels != song::WaveChannels(built, source.index, source.wave))
        {
            return label + " isn't the wave that the Import window shows";
        }
        std::string problem = AppendProblem(built, item, present, same, decoded);
        if (!problem.empty())
        {
            return label + " from its wave file " + problem;
        }

        // The file for each other console goes in converted, with the version of the wave archive's waves.
        for (const sar::Flavor console : song::OtherConsoles(flavor))
        {
            const std::vector<uint8_t> other = song::OtherConsoleFile(built, source, console);
            const std::string name = song::ConsoleName(console);
            const edit::IncomingWave converted = edit::ReadIncomingWaveFile(other, built, item.item);
            if (!converted.converted || converted.converted->flavor != console ||
                converted.converted->version != HeaderVersion(other, console) ||
                converted.converted->size != other.size() || converted.version != version ||
                HeaderVersion(converted.bytes, flavor) != version)
            {
                return label + " doesn't go in converted from its wave file for the " + name;
            }
            problem = AppendProblem(built, item, present, converted, decoded);
            if (!problem.empty())
            {
                return label + " from its wave file for the " + name + " " + problem;
            }
            totals.nw_other_bytes += !std::ranges::equal(converted.bytes, wave_file) ? 1 : 0;
        }

        // The WAV file goes in as 16-bit PCM, with the version of the wave archive's waves.
        const std::vector<uint8_t> wav = song::WaveAsWav(built, source.index, source.wave);
        const edit::IncomingWave from_wav =
            edit::ReadIncomingWave(wav, built, item.item, std::nullopt, sar::WaveEncoding::kPcm16);
        if (from_wav.version != version || HeaderVersion(from_wav.bytes, flavor) != version)
        {
            return label + " doesn't take the version of the wave archive's waves from its WAV file";
        }
        problem = AppendProblem(built, item, present, from_wav, pcm);
        if (!problem.empty())
        {
            return label + " from its WAV file " + problem;
        }

        // The first wave also goes in from its WAV file as DSP-ADPCM.
        problem = k == 0 ? DspAdpcmProblem(built, item, present, std::nullopt, wav, pcm, totals) : std::string();
        if (!problem.empty())
        {
            return label + " " + problem;
        }

        totals.nw_waves++;
        totals.nw_other_versions += HeaderVersion(wave_file, flavor) != version ? 1 : 0;
    }

    // The first wave's file through the Import window's edit: the window reads the file, makes the new wave archive and
    // finds what it changes. Import then makes the edit.
    const auto start = std::chrono::steady_clock::now();
    const edit::IncomingWave incoming = edit::ReadIncomingWaveFile(song::ItemFile(built, sources[0]), built, item.item);
    edit::IncomingFile waves;
    waves.kind = edit::FileKind::kWaveArchive;
    waves.bytes = edit::WaveArchiveWith(built, item.item, std::nullopt, incoming.bytes);
    edit::ChangesOf(file, built, item.item, waves);
    const format::File once = edit::ReplaceFile(file, item.file_id, waves.bytes);
    const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    totals.nw_slowest = std::max(totals.nw_slowest, took.count());
    Archive after;
    const std::string problem = ReplacedProblem(built, groups, item, waves.bytes, once, after);
    if (!problem.empty())
    {
        return "with a new wave " + problem;
    }
    const auto count = static_cast<uint32_t>(present.waves.size());
    if (song::WaveCountOf(*after, item.item) != count + 1 ||
        !SamePcm(song::DecodeItemWave(*after, item.item, count), pcms[0]))
    {
        return "with a new wave plays other samples there";
    }
    if (paired)
    {
        return {};
    }

    // A second new wave after the first, from the first wave's WAV file. The file then saves and opens again.
    const edit::IncomingWave again = edit::ReadIncomingWave(song::WaveAsWav(built, sources[0].index, sources[0].wave),
                                                            *after, item.item, std::nullopt, sar::WaveEncoding::kPcm16);
    const format::File twice =
        edit::ReplaceFile(once, item.file_id, edit::WaveArchiveWith(*after, item.item, std::nullopt, again.bytes));
    const Archive twice_built =
        song::LoadArchive(format::ExportArchive(twice), "twice.bfsar", format::ExportGroupFiles(twice));
    if (again.version != NewWaveVersion(*after, item.item) || song::WaveCountOf(*twice_built, item.item) != count + 2 ||
        !SamePcm(song::DecodeItemWave(*twice_built, item.item, count), pcms[0]) ||
        !SamePcm(song::DecodeItemWave(*twice_built, item.item, count + 1), pcms[0]))
    {
        return "doesn't take a second new wave after the first";
    }
    if (!hokuto::tools::SavesAndOpens(twice, true))
    {
        return "doesn't save and open again with two new waves";
    }
    paired = true;
    totals.nw_pairs++;

    return {};
}

// Checks the new waves of each wave archive (NewWavesProblem). A wave archive that neither the archive nor its group
// files hold, and one whose bytes two file IDs name, stay out: the Import window's edit refuses them.
void CheckNewWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                   const std::string& name, Totals& totals)
{
    bool paired = false;
    for (const FileItem& item : FileItemsOf(built))
    {
        if (item.kind != edit::FileKind::kWaveArchive)
        {
            continue;
        }
        if (Outside(built, item.file_id) || SharedFile(built, item.file_id))
        {
            totals.wave_archives_out++;
            continue;
        }

        totals.wave_archives_beside += Beside(built, item.file_id) ? 1 : 0;

        std::string problem;
        try
        {
            problem = NewWavesProblem(file, built, groups, item, paired, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), NameOf(built, item).c_str(), problem.c_str());
            totals.nw_failed++;
        }
        totals.nw_wave_archives++;
    }
}

// The values that the variables' check gives a variable: its choices, or else 0 and 1.
std::vector<int> CheckedValues(const song::Variable& variable)
{
    return variable.choices.empty() ? std::vector<int>{0, 1} : variable.choices;
}

// A sound whose commands read variables before they set them: the sound, its commands from its file and its start, the
// variables, and the variables that the commands use, a bit each.
struct VariableSound
{
    uint32_t sound = 0;
    Source source;
    std::vector<song::Variable> variables;
    edit::VariableUses uses;
};

// The rounds of the variables' check: a move of each variable, and fixed values from each variable's second value and
// from its last. Only the second round leaves out the variables that the sound's commands read now. Replace… leaves
// them out too.
enum class VariableRound
{
    kMove,
    kSecond,
    kLast,
};

// The map that round `round` gives the variables of `entry`. A move goes to the lowest number of the variable's kind
// that the commands don't use and no other variable takes. A variable without a free number keeps its number.
edit::VariableMap RoundMap(const VariableSound& entry, VariableRound round)
{
    edit::VariableMap map;
    uint64_t taken = entry.uses.reads | entry.uses.sets;
    for (const song::Variable& variable : entry.variables)
    {
        const std::vector<int> values = CheckedValues(variable);
        if (round == VariableRound::kSecond || round == VariableRound::kLast)
        {
            map.fixed[variable.number] = round == VariableRound::kLast ? values.back() : values[values.size() > 1];
            continue;
        }

        const bool global = variable.number >= edit::kSoundVariables;
        for (int to = global ? edit::kSoundVariables : 0; to < (global ? song::kGameVariables : edit::kSoundVariables);
             to++)
        {
            if ((taken >> to & 1u) == 0)
            {
                map.moves[variable.number] = to;
                taken |= uint64_t{1} << to;
                break;
            }
        }
    }

    return map;
}

// What the performer plays of a sound, without the variables that it reads and the values that they start with: an edit
// of the variables changes their numbers.
song::Sequence PlayedNotes(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values)
{
    song::Sequence played = Played(archive, sound, values);
    played.variables.clear();
    played.read_variables.clear();
    played.start_values.clear();

    return played;
}

// Whether a track of sound `sound` of `archive` runs without waits at the core's limit of commands in a tick, with the
// variables' starting values `values`. The performer then warns that the track stopped.
bool RunsAway(const sar::SoundArchive& archive, uint32_t sound, const song::StartValues& values)
{
    namespace export_ = ntdlib::sequence_export;
    const export_::Settings settings = song::SettingsOf(archive);
    export_::BankSet banks(archive, archive.Sounds()[sound], settings);
    const export_::Performance performance = export_::Perform(
        archive, sound, banks, song::PerformOptionsOf(values, {}, archive.ReadOptions().flavor), settings);
    const auto runaway = [](const std::string& warning)
    {
        return warning.find("running without waits") != std::string::npos;
    };

    return std::ranges::any_of(performance.warnings, runaway);
}

// What is wrong with sound `entry.sound` of `after`, the archive after the sound took `map`, or nothing. `before` holds
// what the performer played of each sound of `built` with each set of starting values.
std::string VariablesProblem(const sar::SoundArchive& built, const sar::SoundArchive& after, const VariableSound& entry,
                             const edit::VariableMap& map,
                             std::map<std::pair<uint32_t, song::StartValues>, song::Sequence>& before, Totals& totals)
{
    const auto played_before = [&](const song::StartValues& values) -> const song::Sequence&
    {
        auto found = before.find({entry.sound, values});
        if (found == before.end())
        {
            found = before.emplace(std::pair(entry.sound, values), PlayedNotes(built, entry.sound, values)).first;
        }

        return found->second;
    };

    // A moved variable plays each of its values at its new number as it played them at its old one.
    if (map.fixed.empty())
    {
        for (const song::Variable& variable : entry.variables)
        {
            const auto moved = map.moves.find(variable.number);
            const int number = moved != map.moves.end() ? moved->second : variable.number;
            for (const int value : CheckedValues(variable))
            {
                totals.va_runs++;
                if (PlayedNotes(after, entry.sound, {{number, static_cast<int16_t>(value)}}) !=
                    played_before({{variable.number, static_cast<int16_t>(value)}}))
                {
                    return "plays otherwise with variable " + std::to_string(number) + " at " + std::to_string(value) +
                           " than with variable " + std::to_string(variable.number) + " at that value before";
                }
            }
        }

        return {};
    }

    // Fixed variables play as their values did, whatever values they start with.
    song::StartValues fixed;
    song::StartValues other;
    for (const auto& [number, value] : map.fixed)
    {
        fixed[number] = static_cast<int16_t>(value);
        other[number] = static_cast<int16_t>(value == 0 ? 1 : 0);
    }

    // A track that runs without waits at the core's limit of commands in a tick reads on in the next tick. The command
    // at the start moves where the limit cuts each tick. A sound that draws random numbers then draws them in other
    // ticks.
    totals.va_runs += 2;
    if (PlayedNotes(after, entry.sound, {}) != played_before(fixed))
    {
        if (RunsAway(built, entry.sound, fixed))
        {
            totals.va_runaway++;
            return {};
        }

        return "plays otherwise with its variables fixed than with those values before";
    }
    if (PlayedNotes(after, entry.sound, other) != played_before(fixed))
    {
        return "plays otherwise with other starting values after its variables took fixed values";
    }

    return {};
}

// Checks the variables of Replace… of a sequence (--variables). Up to kVariablePasses sounds of each sequence file take
// each round, one sound of each file in each version.
void CheckVariables(const format::File& file, const sar::SoundArchive& built,
                    std::span<const std::optional<edit::SoundPlace>> places, const std::string& name, Totals& totals)
{
    // The sounds whose commands read variables before they set them, by their sequences, in the order of the sound
    // table.
    constexpr std::size_t kVariablePasses = 4;
    std::map<uint32_t, std::vector<VariableSound>> by_sequence;
    for (std::size_t i = 0; i < built.Sounds().size(); i++)
    {
        const sar::SoundInfo& info = built.Sounds()[i];
        if (info.type != sar::SoundType::kSequence || !places[i] || built.FileData(info.file_id).empty())
        {
            continue;
        }

        Source source = SourceOf(built.FileData(info.file_id), info.start_offset);
        if (!source.start)
        {
            totals.va_no_start++;
            continue;
        }
        std::vector<song::Variable> variables = song::VariablesOf(source.incoming.sequence, *source.start).starting;
        if (variables.empty())
        {
            continue;
        }
        totals.va_reading++;
        std::vector<VariableSound>& sounds = by_sequence[places[i]->sequence];
        if (sounds.size() < kVariablePasses)
        {
            const edit::VariableUses uses = edit::UsesOf(source.incoming.sequence, *source.start);
            sounds.push_back({static_cast<uint32_t>(i), std::move(source), std::move(variables), uses});
        }
    }

    std::map<std::pair<uint32_t, song::StartValues>, song::Sequence> before;
    std::set<uint32_t> checked;
    for (std::size_t pass = 0; pass < kVariablePasses; pass++)
    {
        for (const VariableRound round : {VariableRound::kMove, VariableRound::kSecond, VariableRound::kLast})
        {
            // One sound of each sequence takes the round's map, in one version of the file.
            format::File edited = file;
            std::vector<std::pair<const VariableSound*, edit::VariableMap>> edits;
            for (const auto& [sequence, sounds] : by_sequence)
            {
                if (pass >= sounds.size())
                {
                    continue;
                }

                const VariableSound& entry = sounds[pass];
                const edit::VariableMap map = RoundMap(entry, round);
                const uint32_t taken =
                    round == VariableRound::kSecond ? static_cast<uint32_t>(entry.uses.reads & 0xFFFF) : 0;
                try
                {
                    const edit::MappedVariables mapped =
                        edit::WithVariables(entry.source.incoming.sequence, *entry.source.start, map, taken);
                    const uint32_t tracks =
                        edit::TracksToAdd(edited, places, entry.sound, mapped.sequence, mapped.start);
                    edited = edit::ReplaceSequence(edited, places, entry.sound, mapped.sequence, mapped.start, {},
                                                   "Check", tracks);
                    edits.emplace_back(&entry, map);
                    totals.va_moved += map.moves.size();
                    totals.va_fixed += mapped.holders.size();
                    for (const auto& [number, holder] : mapped.holders)
                    {
                        totals.va_held += holder != number ? 1 : 0;
                    }
                }
                catch (const std::invalid_argument& error)
                {
                    totals.va_refused++;
                    totals.va_reasons.insert(error.what());
                }
            }
            if (edits.empty())
            {
                continue;
            }

            // The round's version of the file saves and exports. Each of its edits fails when it doesn't.
            Archive after;
            std::string failure;
            try
            {
                hokuto::tools::CheckSaved(edited, true);
                after =
                    song::LoadArchive(format::ExportArchive(edited), "after.bfsar", format::ExportGroupFiles(edited));
            }
            catch (const std::exception& error)
            {
                failure = std::string("fails: ") + error.what();
            }

            for (const auto& [entry, map] : edits)
            {
                std::string problem;
                try
                {
                    problem = failure.empty() ? VariablesProblem(built, *after, *entry, map, before, totals) : failure;
                }
                catch (const std::exception& error)
                {
                    problem = std::string("fails: ") + error.what();
                }
                if (!problem.empty())
                {
                    std::printf("%s: %s %s\n", name.c_str(), built.Sounds()[entry->sound].name.c_str(),
                                problem.c_str());
                    totals.va_failed++;
                }
                checked.insert(entry->sound);
            }
        }
    }
    totals.va_checked = checked.size();
}

// Whether `b`, a stream made again from the WAV file of `a`, plays the samples that `a` plays: the same samples up to
// the end of `a`'s loop, then the samples from the loop's start that the later start of `b`'s loop moved after the end.
bool PlaysSame(const sar::Pcm& a, const sar::Pcm& b)
{
    if (a.channels.size() != b.channels.size() || a.sample_rate != b.sample_rate || a.loop != b.loop ||
        b.loop_start < a.loop_start)
    {
        return false;
    }

    const uint32_t moved = a.loop ? b.loop_start - a.loop_start : 0;
    if (b.Length() != a.Length() + moved)
    {
        return false;
    }
    for (std::size_t c = 0; c < a.channels.size(); c++)
    {
        const std::vector<int16_t>& old = a.channels[c];
        const std::vector<int16_t>& now = b.channels[c];
        if (old.size() < a.Length() || now.size() < b.Length())
        {
            return false;
        }
        for (uint32_t i = 0; i < b.Length(); i++)
        {
            if (now[i] != (i < a.Length() ? old[i] : old[a.loop_start + (i - a.Length())]))
            {
                return false;
            }
        }
    }

    return true;
}

// Whether two sounds hold the same settings that a new sound copies from a model of any kind.
bool SameSettings(const sar::SoundInfo& a, const sar::SoundInfo& b)
{
    return a.volume == b.volume && a.pan_mode == b.pan_mode && a.pan_curve == b.pan_curve &&
           a.player_id == b.player_id && a.player_priority == b.player_priority &&
           a.actor_player_id == b.actor_player_id;
}

// What is wrong with `added`, the version of `base` whose archive gains stream sound `name`, a copy of sound `model`
// that plays `made` from a new file, or nothing. `built` is the archive that `base` builds.
std::string NewStreamProblem(const format::File& base, const sar::SoundArchive& built, const format::File& added,
                             uint32_t model, const std::string& name, const edit::NewStream& made)
{
    const std::vector<uint8_t> bytes = format::ExportArchive(added);
    const Archive after = song::LoadArchive(bytes, "after.bfsar", format::ExportGroupFiles(added));
    const auto sound = static_cast<uint32_t>(built.Sounds().size());
    const auto file_id = static_cast<uint32_t>(built.FileCount());
    if (after->Sounds().size() != sound + 1 || after->FileCount() != file_id + 1)
    {
        return "doesn't give the archive one sound and one file more";
    }

    // The new sound, its file and its stream. An export writes the stream at the new path.
    const sar::SoundInfo& info = after->Sounds()[sound];
    const sar::SoundInfo& copied = built.Sounds()[model];
    if (info.name != name || info.file_id != file_id || info.type != sar::SoundType::kStream ||
        !SameSettings(info, copied) || info.stream != made.details ||
        (copied.type == sar::SoundType::kStream && !SameValues(info, copied)))
    {
        return "gives the new sound other values than the model's and the stream's";
    }
    const std::vector<uint32_t> files = {file_id};
    if (after->ExternalPath(file_id) != made.path || song::StreamPaths(*after, files).front() != made.path)
    {
        return "gives the new file another path";
    }
    const auto found = added.native->streams.find(file_id);
    if (found == added.native->streams.end() || *found->second.bytes != made.bytes || found->second.from_game ||
        added.native->streams.size() != base.native->streams.size() + 1)
    {
        return "doesn't keep the new stream as Hokuto's";
    }
    for (const auto& [id, stream] : base.native->streams)
    {
        const auto kept = added.native->streams.find(id);
        if (kept == added.native->streams.end() || kept->second != stream)
        {
            return "changes stream file " + std::to_string(id);
        }
    }

    // The other items and files.
    for (uint32_t i = 0; i < sound; i++)
    {
        const sar::SoundInfo& old = built.Sounds()[i];
        const sar::SoundInfo& now = after->Sounds()[i];
        if (now.name != old.name || now.file_id != old.file_id || now.start_offset != old.start_offset ||
            now.allocate_track_flags != old.allocate_track_flags || !SameValues(now, old) || now.stream != old.stream)
        {
            return "changes " + old.name;
        }
    }
    for (uint32_t id = 0; id < built.FileCount(); id++)
    {
        if (!std::ranges::equal(built.FileData(id), after->FileData(id)) ||
            built.ExternalPath(id) != after->ExternalPath(id))
        {
            return "changes file " + std::to_string(id);
        }
    }
    if (NamesOf(after->Banks()) != NamesOf(built.Banks()) ||
        NamesOf(after->WaveArchives()) != NamesOf(built.WaveArchives()) ||
        NamesOf(after->Players()) != NamesOf(built.Players()) || NamesOf(after->Groups()) != NamesOf(built.Groups()))
    {
        return "changes the names of other items";
    }

    return {};
}

// What is wrong with stream file `id` of `file`, or nothing. Stream sound `model` plays the stream. `built` is the
// archive that `file` builds. With `second`, a second new sound goes in after the first, and the file saves and opens
// again.
std::string StreamProblem(const format::File& file, const sar::SoundArchive& built, uint32_t id, uint32_t model,
                          bool second, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::vector<uint8_t>& bytes = *file.native->streams.at(id).bytes;
    const std::vector<uint8_t> archive = format::ExportArchive(file);

    // The stream itself, and its file for the other console. A stream of the version that the archive's streams take
    // comes back with the same bytes. A 3DS stream comes back with its samples: the Wii U's file drops the samples past
    // its end, and its original loop.
    if (edit::ReplaceStream(file, id, bytes) != file)
    {
        return "doesn't give the file back with its stream";
    }
    const bool ctr = flavor == sar::Flavor::kCtr;
    const uint32_t version = edit::IncomingStreamVersion(file, flavor);
    const uint32_t present = sar::StreamFile::Read(bytes).version;
    const sar::Pcm decoded = song::DecodeStreamFile(bytes);
    const sar::Flavor other_console = song::OtherConsole(flavor);
    const std::vector<uint8_t> other_file = song::ConvertStreamFile(
        bytes, other_console, song::ExportVersions(song::ItemKind::kStream, other_console).file);
    const edit::IncomingStream converted = edit::ReadIncomingStreamFile(other_file, flavor, version);
    const bool exact = present == version && !ctr;
    if (!converted.converted || converted.version != version ||
        (exact ? converted.bytes != bytes : !PlaysSame(decoded, song::DecodeStreamFile(converted.bytes))))
    {
        return "comes back from the other console with other bytes";
    }

    // On the Wii U, a stream of a later version than 3.0.0 goes in written again in 3.0.0, and on the 3DS one of a
    // later version than 2.0.0.0 in 2.0.0.0. The new stream then goes in as it is.
    const uint32_t oldest = flavor == sar::Flavor::kCafe ? 0x00030000 : ctr ? 0x02000000 : 0;
    if (oldest != 0 && present > oldest)
    {
        const std::string text = song::VersionText(flavor, oldest);
        const edit::IncomingStream old = edit::ReadIncomingStreamFile(bytes, flavor, oldest);
        if (!old.converted || old.converted->flavor != flavor || old.converted->version != present ||
            old.version != oldest || !PlaysSame(decoded, song::DecodeStreamFile(old.bytes)))
        {
            return "plays other samples written again in version " + text;
        }
        const edit::IncomingStream again = edit::ReadIncomingStreamFile(old.bytes, flavor, version);
        if (again.converted || again.bytes != old.bytes)
        {
            return "doesn't go in as it is in version " + text;
        }
        totals.st_written++;
    }

    // The WAV file in 16-bit PCM: the same samples, and the archive as it was. Where the sound plays the tracks of its
    // stream file, the new stream takes the present stream's.
    const edit::StreamChanges changes = edit::ChangesOfStream(file, built, model);
    const std::vector<uint8_t> wav = song::StreamAsWav(bytes);
    const edit::IncomingStream pcm =
        edit::WithSoundTracks(edit::ReadIncomingStreamWav(wav, flavor, version, sar::WaveEncoding::kPcm16), changes);
    if (!PlaysSame(decoded, song::DecodeStreamFile(pcm.bytes)))
    {
        return "plays other samples from its WAV file in 16-bit PCM";
    }
    if (changes.tracks_in_file && sar::StreamFile::Read(pcm.bytes).tracks != sar::StreamFile::Read(bytes).tracks)
    {
        return "doesn't keep the present stream's tracks from its WAV file";
    }
    totals.st_tracked += changes.tracks_in_file ? 1 : 0;
    totals.st_moved += pcm.loop_moved > 0 ? 1 : 0;
    const format::File replaced = edit::ReplaceStream(file, id, pcm.bytes);
    hokuto::tools::CheckSaved(replaced, true);
    if (format::ExportArchive(replaced) != archive || replaced.native->streams.at(id).from_game ||
        *replaced.native->streams.at(id).bytes != pcm.bytes)
    {
        return "doesn't keep the stream from its WAV file as Hokuto's";
    }

    // The WAV file in DSP-ADPCM.
    const edit::IncomingStream adpcm = edit::ReadIncomingStreamWav(wav, flavor, version);
    if (adpcm.signal_to_noise)
    {
        totals.st_ratios.push_back(*adpcm.signal_to_noise);
    }
    else
    {
        totals.st_exact++;
    }

    // A new sound that copies the sound, with the stream from its file. The edit refuses a new sound in an archive
    // without names, and the tool counts the stream apart.
    const std::string name = FreeTableName(*file.native->tables, "HOKUTO_NEW_STREAM");
    const edit::NewStream made =
        edit::MakeNewStream(built, model, name, edit::ReadIncomingStreamFile(bytes, flavor, version));
    if (!sar::TablesOf(archive, flavor).strings)
    {
        try
        {
            edit::AddStreamSound(file, model, name, made);
        }
        catch (const std::invalid_argument& error)
        {
            if (std::string(error.what()) == "The archive holds no names. Hokuto can't name a new sound in it.")
            {
                totals.st_nameless++;
                return {};
            }
        }
        return "takes a new sound in an archive without names";
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = edit::AddStreamSound(file, model, name, made);
    totals.st_slowest =
        std::max(totals.st_slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = NewStreamProblem(file, built, added, model, name, made); !problem.empty())
    {
        return problem;
    }
    totals.st_same_details += made.details == built.Sounds()[model].stream ? 1 : 0;
    if (!second)
    {
        return {};
    }

    // A second new sound from the WAV file, with a sound of another kind as its model where the archive holds one. The
    // file saves and opens again.
    const std::vector<uint8_t> added_bytes = format::ExportArchive(added);
    const Archive after = song::LoadArchive(added_bytes, "after.bfsar", format::ExportGroupFiles(added));
    const auto other = std::ranges::find_if(
        after->Sounds(), [](const sar::SoundInfo& sound) { return sound.type != sar::SoundType::kStream; });
    const auto second_model =
        other != after->Sounds().end() ? static_cast<uint32_t>(other - after->Sounds().begin()) : model;
    const std::string second_name = FreeTableName(*added.native->tables, "HOKUTO_NEW_STREAM");
    const edit::NewStream second_made = edit::MakeNewStream(*after, second_model, second_name, pcm);
    const format::File twice = edit::AddStreamSound(added, second_model, second_name, second_made);
    if (std::string problem = NewStreamProblem(added, *after, twice, second_model, second_name, second_made);
        !problem.empty())
    {
        return "after a first new sound, " + problem;
    }
    if (!hokuto::tools::SavesAndOpens(twice, true))
    {
        return "doesn't save and open again with two new sounds";
    }
    totals.st_pairs++;

    return {};
}

// The ratio of signal to noise of the samples of `got` from `offset` on against `expected`'s, in decibels, over every
// channel up to `expected`'s end. Nothing where `got` holds other channels or too few samples, or `expected` none.
std::optional<double> SignalToNoise(const sar::Pcm& got, std::size_t offset, const sar::Pcm& expected)
{
    if (got.channels.size() != expected.channels.size() || expected.channels.empty())
    {
        return std::nullopt;
    }

    double signal = 0.0;
    double noise = 0.0;
    for (std::size_t channel = 0; channel < expected.channels.size(); channel++)
    {
        const std::vector<int16_t>& want = expected.channels[channel];
        const std::vector<int16_t>& have = got.channels[channel];
        if (have.size() < offset + want.size())
        {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < want.size(); i++)
        {
            const double difference = static_cast<double>(have[offset + i]) - want[i];
            signal += static_cast<double>(want[i]) * want[i];
            noise += difference * difference;
        }
    }
    if (signal <= 0.0)
    {
        return std::nullopt;
    }

    return noise > 0.0 ? 10.0 * std::log10(signal / noise) : 1e9;
}

// The lowest ratio of signal to noise that an AAC stream encoded again from its WAV file or its file for the Wii U may
// take, in decibels, and how far above the ratios a sample before and after the encoder's delay it has to lie. A stream
// a sample off can still lie above the floor: the ratio has to peak at the delay.
constexpr double kLeastAacRatio = 15.0;
constexpr double kAacAlignment = 3.0;

// What is wrong with `replaced`, `model`'s file after Replace… of AAC stream `id`, or nothing. Its model has to be
// `model` with `bytes` in the stream's place, and `loop` in the entries of the sounds that play the stream.
std::string AacModelProblem(const format::Model& model, const format::File& replaced, uint32_t id,
                            const std::vector<uint8_t>& bytes, const std::optional<song::AacLoop>& loop)
{
    hokuto::tools::CheckSaved(replaced);
    format::Model after = format::ModelOf(replaced);
    const auto stream = after.streams.find(id);
    if (stream == after.streams.end() || stream->second != bytes)
    {
        return "doesn't hold the new stream";
    }
    if (const auto game = model.streams.find(id); game != model.streams.end())
    {
        stream->second = game->second;
    }
    else
    {
        after.streams.erase(stream);
    }
    if (after.archive.sounds.size() != model.archive.sounds.size())
    {
        return "changes the sound table";
    }
    for (std::size_t i = 0; i < after.archive.sounds.size(); i++)
    {
        sar::SoundModel& entry = after.archive.sounds[i];
        const sar::SoundModel& game = model.archive.sounds[i];
        if (entry.type != sar::SoundType::kStream || entry.file_id != id || !entry.stream_extension)
        {
            continue;
        }
        const sar::StreamExtension& extension = *entry.stream_extension;
        if (((extension.stream_type & 0x100) != 0) != loop.has_value() ||
            (loop && (extension.loop_start != loop->start || extension.loop_end != loop->end)))
        {
            return "gives " + entry.name + " another loop";
        }
        entry.stream_extension = game.stream_extension;
        entry.stream_options = game.stream_options;
    }

    return after == model ? std::string() : "changes other values of the archive";
}

// What is wrong with an encoded AAC stream `encoded` from samples `source`, or nothing: its samples have to come after
// the encoder's delay, and `loop` has to be the loop of `source` after the delay. Counts its ratio of signal to noise.
std::string EncodedAacProblem(const edit::IncomingAac& encoded, const sar::Pcm& source,
                              std::optional<song::AacLoop>& loop, Totals& totals)
{
    loop = source.loop ? std::optional(song::AacLoop{encoded.delay + source.loop_start,
                                                     encoded.delay + static_cast<uint32_t>(source.loop_end)})
                       : std::nullopt;
    if (!encoded.encoded || encoded.after_loop != 0 || source.channels.at(0).size() != source.loop_end ||
        encoded.length != encoded.delay + source.loop_end || encoded.loop.has_value() != loop.has_value() ||
        (loop && (encoded.loop->start != loop->start || encoded.loop->end != loop->end)))
    {
        return "gives another length or loop";
    }
    const sar::Pcm decoded = song::DecodeAdts(encoded.bytes);
    const std::optional<double> ratio = SignalToNoise(decoded, encoded.delay, source);
    const std::optional<double> before = SignalToNoise(decoded, encoded.delay - 1, source);
    const std::optional<double> after = SignalToNoise(decoded, encoded.delay + 1, source);
    if (!ratio || *ratio < kLeastAacRatio || encoded.delay == 0)
    {
        return "decodes to other samples";
    }
    if (*ratio < before.value_or(0.0) + kAacAlignment || *ratio < after.value_or(0.0) + kAacAlignment)
    {
        return "decodes to its samples at another delay";
    }
    totals.st_aac_ratios.push_back(*ratio);
    totals.st_aac_delay = encoded.delay;

    return {};
}

// What is wrong with Replace… of AAC stream `id` of `file`, or nothing (edit::ReplaceAacStream). Stream sound `sound`
// plays the stream. `built` is the archive that `file` builds. The stream's file goes in as it is, with the present
// loop, and the file in the model form has to hold the game's model. The stream's WAV file and its file for the Wii U
// from Export… go in encoded by the system's encoder. The new stream has to decode to the samples that went in, after
// the encoder's delay, and the entries of the sounds that play it have to give the loop after the delay. Every other
// value of the model has to stay.
std::string AacStreamProblem(const format::File& file, const sar::SoundArchive& built, uint32_t id, uint32_t sound,
                             Totals& totals)
{
    const std::optional<edit::AacChanges> changes = edit::ChangesOfAacStream(file, built, sound);
    const std::optional<song::AacStream> aac = song::AacStreamOf(built, sound);
    if (!changes || !aac || changes->file != id || !changes->present)
    {
        return "plays no AAC stream that the file holds";
    }
    const std::vector<uint8_t>& present = *file.native->streams.at(id).bytes;
    const format::Model model = format::ModelOf(file);
    const auto problem_of = [&model, id](const format::File& replaced, const std::vector<uint8_t>& bytes,
                                         const std::optional<song::AacLoop>& loop)
    {
        return AacModelProblem(model, replaced, id, bytes, loop);
    };

    // The stream's file, with the present loop.
    const edit::IncomingAac itself = edit::ReadIncomingAac(present, *changes);
    if (itself.encoded || itself.bytes != present || itself.present_loop != changes->loop.has_value())
    {
        return "doesn't take its file as it is";
    }
    if (std::string problem = problem_of(edit::ReplaceAacStream(file, *changes, itself), present, changes->loop);
        !problem.empty())
    {
        return "with its file, " + problem;
    }

    // The stream's WAV file and its file for the Wii U, encoded again. Each holds the stream's samples up to the end of
    // its loop, with the loop. The Wii U's file moves the loop's start forward to a block, and the samples between
    // repeat after the loop's end.
    const std::vector<uint8_t> wav = song::StreamAsWav(present, *aac);
    const std::vector<uint8_t> wii_u = song::ConvertStreamFile(
        present, sar::Flavor::kCafe, song::ExportVersions(song::ItemKind::kStream, sar::Flavor::kCafe).file, *aac);
    for (const auto& [what, bytes] : {std::pair{"its WAV file", &wav}, std::pair{"its file for the Wii U", &wii_u}})
    {
        const std::string from = std::string("from ") + what;
        const sar::Pcm source = bytes == &wav ? song::DecodeStreamFile(present, *aac) : song::DecodeStreamFile(*bytes);
        const edit::IncomingAac encoded = edit::ReadIncomingAac(*bytes, *changes);
        std::optional<song::AacLoop> loop;
        if (std::string problem = EncodedAacProblem(encoded, source, loop, totals); !problem.empty())
        {
            return from + ", " + problem;
        }
        if (std::string problem = problem_of(edit::ReplaceAacStream(file, *changes, encoded), encoded.bytes, loop);
            !problem.empty())
        {
            return from + ", " + problem;
        }
    }

    totals.st_aac_replaced++;

    return {};
}

// What is wrong with Replace… of the AAC stream of stream sound `sound` of `file`, or nothing. The file lacks the
// stream. `built` is the archive that `file` builds. `wav` is a WAV file of 1 or 2 channels at 44,100 or 48,000 Hz. The
// new stream has to be AAC, at 128 kbit/s, with the WAV file's rate and the 1 or 2 channels that the sound's entry
// gives, or else the WAV file's. A mono file fills both channels of a stereo stream, and a stereo file goes into a mono
// stream as the mean of its channels. The new stream has to decode to those samples after the encoder's delay, and the
// entries of the sounds that play it have to give the loop after the delay. Every other value of the model has to stay.
std::string MissingAacProblem(const format::File& file, const sar::SoundArchive& built, uint32_t sound,
                              const std::vector<uint8_t>& wav, Totals& totals)
{
    const std::optional<edit::AacChanges> changes = edit::ChangesOfAacStream(file, built, sound);
    if (!changes || changes->present || changes->bit_rate != 128000 || file.native->streams.contains(changes->file))
    {
        return "isn't an AAC stream that the file lacks";
    }
    const seqlib::WavContents contents = seqlib::ReadWav(wav);
    sar::Pcm source;
    source.channels = contents.pcm.channels;
    source.sample_rate = contents.rate;
    source.loop = contents.pcm.loop;
    source.loop_start = contents.pcm.loop_start;
    source.loop_end = contents.pcm.loop ? contents.pcm.loop_end : static_cast<uint32_t>(source.channels.at(0).size());
    for (std::vector<int16_t>& channel : source.channels)
    {
        channel.resize(source.loop_end);
    }
    const uint16_t entry = built.Sounds().at(sound).stream.channel_count;
    const std::size_t channels = entry == 1 || entry == 2 ? entry : source.channels.size();
    if (source.channels.size() == 1 && channels == 2)
    {
        source.channels.push_back(source.channels[0]);
    }
    else if (source.channels.size() == 2 && channels == 1)
    {
        for (std::size_t i = 0; i < source.channels[0].size(); i++)
        {
            source.channels[0][i] = static_cast<int16_t>((source.channels[0][i] + source.channels[1][i]) / 2);
        }
        source.channels.resize(1);
    }
    const edit::IncomingAac encoded = edit::ReadIncomingAac(wav, *changes);
    if (encoded.stream_channels != channels || encoded.stream_rate != source.sample_rate)
    {
        return "takes other channels or another rate from a WAV file";
    }
    std::optional<song::AacLoop> loop;
    if (std::string problem = EncodedAacProblem(encoded, source, loop, totals); !problem.empty())
    {
        return "from a WAV file, " + problem;
    }
    if (std::string problem = AacModelProblem(format::ModelOf(file), edit::ReplaceAacStream(file, *changes, encoded),
                                              changes->file, encoded.bytes, loop);
        !problem.empty())
    {
        return "from a WAV file, " + problem;
    }
    totals.st_aac_missing++;

    return {};
}

// The streams beside `built`, the archive that `file` builds. Each stream goes in with the first stream sound that
// plays it (StreamProblem, and AacStreamProblem for an AAC stream).
void CheckStreams(const format::File& file, const sar::SoundArchive& built, const std::string& name, Totals& totals)
{
    bool second = true;
    for (const auto& [id, stream] : file.native->streams)
    {
        const auto plays = [id](const sar::SoundInfo& sound)
        {
            return sound.type == sar::SoundType::kStream && sound.file_id == id;
        };
        const auto model = std::ranges::find_if(built.Sounds(), plays);
        if (model == built.Sounds().end())
        {
            totals.st_unplayed++;
            continue;
        }

        std::string problem;
        try
        {
            const auto sound = static_cast<uint32_t>(model - built.Sounds().begin());
            if (song::IsAdts(*stream.bytes))
            {
                problem = AacStreamProblem(file, built, id, sound, totals);
                totals.st_channels += song::DecodeAdts(*stream.bytes).channels.size();
            }
            else
            {
                problem = StreamProblem(file, built, id, sound, std::exchange(second, false), totals);
                totals.st_channels += sar::StreamFile::Read(*stream.bytes).channels.size();
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), model->name.c_str(), problem.c_str());
            totals.st_failed++;
        }
        totals.st_streams++;
    }

    // The first stream sound whose entry marks an AAC stream that the file lacks takes the WAV file of the first AAC
    // stream that the file holds (MissingAacProblem).
    std::optional<std::vector<uint8_t>> wav;
    for (uint32_t sound = 0; sound < built.Sounds().size() && !wav; sound++)
    {
        const std::optional<uint32_t> id = song::StreamFileOf(built, sound);
        const std::optional<song::AacStream> aac = song::AacStreamOf(built, sound);
        const auto held = id ? file.native->streams.find(*id) : file.native->streams.end();
        if (aac && held != file.native->streams.end() && song::IsAdts(*held->second.bytes))
        {
            try
            {
                wav = song::StreamAsWav(*held->second.bytes, *aac);
            }
            catch (const std::exception& error)
            {
                std::printf("%s: the stream of %s doesn't go out as a WAV file: %s\n", name.c_str(),
                            built.Sounds()[sound].name.c_str(), error.what());
                totals.st_failed++;
                break;
            }
        }
    }
    for (uint32_t sound = 0; sound < built.Sounds().size() && wav; sound++)
    {
        const std::optional<uint32_t> id = song::StreamFileOf(built, sound);
        if (!id || file.native->streams.contains(*id) || !song::AacStreamOf(built, sound))
        {
            continue;
        }
        std::string problem;
        try
        {
            problem = MissingAacProblem(file, built, sound, *wav, totals);
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: %s without its stream %s\n", name.c_str(), built.Sounds()[sound].name.c_str(),
                        problem.c_str());
            totals.st_failed++;
        }
        break;
    }
}

// What is wrong with `bytes`, a stream file of a donor, as a new stream sound of the archive of `file` with sound
// `model` as its model, or nothing. `built` is the archive that `file` builds, and `archive` the archive that it
// exports. With `save`, the file with the new sound saves and opens again.
std::string DonorStreamProblem(const format::File& file, const sar::SoundArchive& built,
                               std::span<const uint8_t> archive, uint32_t model, const std::vector<uint8_t>& bytes,
                               bool save, Totals& totals)
{
    const sar::Flavor flavor = built.ReadOptions().flavor;
    const std::string name = FreeTableName(*file.native->tables, "HOKUTO_NEW_STREAM");
    const edit::IncomingStream incoming =
        edit::ReadIncomingStreamFile(bytes, flavor, edit::IncomingStreamVersion(file, flavor));

    // A 3DS archive whose layout of stream sounds is unknown, and an archive without names, refuse the new sound.
    const uint32_t version = built.Version();
    const bool unknown = flavor == sar::Flavor::kCtr && version != 0x02000000 && version != 0x02020000 &&
                         version != 0x02030000 && version < 0x02030200;
    const bool nameless = !sar::TablesOf(archive, flavor).strings;
    if (unknown || nameless)
    {
        try
        {
            edit::AddStreamSound(file, model, name, edit::MakeNewStream(built, model, name, incoming));
        }
        catch (const std::invalid_argument& error)
        {
            const std::string text = error.what();
            if (unknown && text.starts_with("Hokuto can't add a stream sound to a 3DS archive of version"))
            {
                totals.st_unknown++;
                return {};
            }
            if (!unknown && text == "The archive holds no names. Hokuto can't name a new sound in it.")
            {
                totals.st_nameless++;
                return {};
            }
            return "is refused: " + text;
        }
        return unknown ? "takes a new sound in a 3DS archive whose layout of stream sounds is unknown"
                       : "takes a new sound in an archive without names";
    }

    // The new stream plays the donor's samples. Where the sound plays the tracks of its stream file, a stream without
    // tracks takes a track for each pair of its channels.
    const edit::NewStream made = edit::MakeNewStream(built, model, name, incoming);
    if (!PlaysSame(song::DecodeStreamFile(bytes), song::DecodeStreamFile(made.bytes)))
    {
        return "plays other samples than the donor's stream";
    }
    if (made.tracks_in_file != edit::TracksInFiles(built))
    {
        return "gives the stream its tracks otherwise than the archive's sounds play them";
    }
    if (made.tracks_in_file && !made.file_tracks)
    {
        const sar::StreamFile stream = sar::StreamFile::Read(made.bytes);
        if (!stream.tracks || stream.tracks->size() != (stream.channels.size() + 1) / 2)
        {
            return "doesn't take a track for each pair of its channels";
        }
        totals.st_paired++;
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = edit::AddStreamSound(file, model, name, made);
    totals.st_slowest =
        std::max(totals.st_slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = NewStreamProblem(file, built, added, model, name, made); !problem.empty())
    {
        return problem;
    }
    totals.st_same_details += made.details == built.Sounds()[model].stream ? 1 : 0;
    if (save && !hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again with the new sound";
    }

    return {};
}

// The streams beside the archive of `donor` as new stream sounds of `built`, the archive that `file` builds
// (DonorStreamProblem). The model alternates between the archive's first stream sound and its first sound of another
// kind.
void CheckDonorStreams(const format::File& file, const sar::SoundArchive& built, const format::File& donor,
                       const std::string& name, Totals& totals)
{
    const auto first = [&built](bool stream) -> std::optional<uint32_t>
    {
        const auto of_kind = [stream](const sar::SoundInfo& sound)
        {
            return (sound.type == sar::SoundType::kStream) == stream;
        };
        const auto found = std::ranges::find_if(built.Sounds(), of_kind);
        if (found == built.Sounds().end())
        {
            return std::nullopt;
        }

        return static_cast<uint32_t>(found - built.Sounds().begin());
    };
    const std::optional<uint32_t> stream_model = first(true);
    const std::optional<uint32_t> other_model = first(false);
    if (!stream_model && !other_model)
    {
        return;
    }
    const std::vector<uint8_t> archive = format::ExportArchive(file);
    bool turn = false;
    bool save = true;
    for (const auto& [id, stream] : donor.native->streams)
    {
        if (song::IsAdts(*stream.bytes))
        {
            totals.st_aac++;
            continue;
        }

        const uint32_t model =
            std::exchange(turn, !turn) ? other_model.value_or(*stream_model) : stream_model.value_or(*other_model);
        std::string problem;
        try
        {
            problem =
                DonorStreamProblem(file, built, archive, model, *stream.bytes, std::exchange(save, false), totals);
            totals.st_channels += sar::StreamFile::Read(*stream.bytes).channels.size();
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }
        if (!problem.empty())
        {
            std::printf("%s: the donor's stream file %u %s\n", name.c_str(), id, problem.c_str());
            totals.st_failed++;
        }
        totals.st_streams++;
    }
}

// Prints the ratios of signal to noise of the waves from their WAV files as DSP-ADPCM, by tens of decibels. With
// `half_rates`, also prints the waves that came back from their WAV files at half their rates.
void PrintDspAdpcm(const std::string& name, const Totals& totals, bool half_rates)
{
    const std::vector<double>& ratios = totals.adpcm_ratios;
    const auto between = [&ratios](double low, double high)
    {
        return std::count_if(ratios.begin(), ratios.end(), [&](double ratio) { return ratio >= low && ratio < high; });
    };

    const double least = ratios.empty() ? 0.0 : *std::min_element(ratios.begin(), ratios.end());
    std::printf(
        "%s: As DSP-ADPCM, %zu waves came back exactly, and %zu with a signal-to-noise ratio of at least %.1f dB: %td "
        "under 20 dB, %td from 20 to 30 dB, %td from 30 to 40 dB and %td from 40 dB. %zu loops moved forward to a "
        "frame. The WAV files took %.1f s to read as DSP-ADPCM, for %zu samples.\n",
        name.c_str(), totals.adpcm_exact, ratios.size(), least, between(-1e9, 20.0), between(20.0, 30.0),
        between(30.0, 40.0), between(40.0, 1e9), totals.adpcm_moved, totals.adpcm_seconds, totals.adpcm_samples);
    if (!half_rates)
    {
        return;
    }

    std::printf(
        "%s: %zu waves came back from their WAV files at half their rates, with %zu samples in all channels. "
        "Their WAV files took %.1f s to read. %zu waves at 8,000 Hz or less kept their rates. %zu looped waves "
        "came back from their WAV files without loops, with the present waves' loops.\n",
        name.c_str(), totals.resampled, totals.resampled_samples, totals.resampled_seconds, totals.resampled_low,
        totals.loops_offered);
}

// The Wii's part of the check. An archive of the Wii imports with the game's build of NW4R SND, from the main.dol of
// the extracted disc that holds the archive, or else with the build that the archive's version gives. Its sequences
// read and play by the rules of that build.

using WiiArchive = std::shared_ptr<const ntdlib::rsar::SoundArchive>;

// What the Wii's part counted.
struct WiiTotals
{
    std::size_t sounds = 0;        // sequence sounds that took their files again
    std::size_t crossed = 0;       // sequence sounds that took another sound's file
    std::size_t failed = 0;        // sequence sounds that fail
    std::size_t in_bytes = 0;      // sequence sounds whose files stay in the archive's bytes
    std::size_t unlabelled = 0;    // sounds that start past their file's first command, where it has no label
    std::size_t untracked = 0;     // sounds that open tracks that they don't allocate
    std::size_t loops_moved = 0;   // sounds whose loop the performer found a whole number of passes away
    std::size_t other_lengths = 0; // sounds whose values decide another length for the new notes
    std::size_t files = 0;         // files of banks, wave archives and wave sounds that took their bytes again
    std::size_t files_crossed = 0;
    std::size_t files_failed = 0;
    std::size_t copies = 0;       // copies of files that took new bytes
    std::size_t other_form = 0;   // next files left out: they keep their waves otherwise
    std::size_t without_next = 0; // files without a next file of their kind and form
    std::size_t waves = 0;        // waves that came back from their wave files and their WAV files
    std::size_t wave_archives = 0;
    std::size_t wave_archives_failed = 0;
    std::size_t waves_wide = 0;    // waves of more than two channels: a WAV file of Export… doesn't hold them
    std::size_t loops_forward = 0; // DSP-ADPCM waves whose loops moved forward to a frame
    std::size_t empty_loops = 0;   // waves whose loops hold no samples: a WAV file can't hold such a loop
    std::vector<double> ratios;    // the ratios of signal to noise of the waves in DSP-ADPCM, in decibels
    std::size_t exact = 0;         // waves that came back exactly in DSP-ADPCM
    std::map<std::string, std::size_t> refused; // Replace…'s refusals, by their reasons
};

// The copies of file `id` in the groups of the Wii's `archive`, read from `bytes`, or of its wave data with
// `wave_data`, in the order of the groups. A copy that runs past the end of an archive that is cut short stays out.
std::vector<std::vector<uint8_t>> WiiCopies(std::span<const uint8_t> bytes, const ntdlib::rsar::SoundArchive& archive,
                                            uint32_t id, bool wave_data)
{
    std::vector<std::vector<uint8_t>> copies;
    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            const std::size_t start = wave_data ? std::size_t{group.wave_data_offset} + item.wave_data_offset
                                                : std::size_t{group.offset} + item.offset;
            const std::size_t size = wave_data ? item.wave_data_size : item.size;
            if (item.file_id == id && group.internal && start <= bytes.size() && size <= bytes.size() - start)
            {
                copies.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(start + size));
            }
        }
    }

    return copies;
}

// The IDs of the files that the groups of the Wii's `archive` hold, each once, in order.
std::vector<uint32_t> WiiFileIds(const ntdlib::rsar::SoundArchive& archive)
{
    std::set<uint32_t> ids;
    for (const ntdlib::rsar::SoundArchive::Group& group : archive.Groups())
    {
        for (const ntdlib::rsar::SoundArchive::Group::Item& item : group.items)
        {
            ids.insert(item.file_id);
        }
    }

    return {ids.begin(), ids.end()};
}

// What is wrong with the files of `after`, read from `after_bytes`, against those of `built`, read from `built_bytes`,
// or nothing. File `changed` may differ, in its file with `file` and in its wave data with `wave_data`. Every other
// file has to keep its bytes and its wave data in every copy.
std::string OtherWiiFilesProblem(std::span<const uint8_t> built_bytes, const ntdlib::rsar::SoundArchive& built,
                                 std::span<const uint8_t> after_bytes, const ntdlib::rsar::SoundArchive& after,
                                 uint32_t changed, bool file, bool wave_data)
{
    for (const uint32_t id : WiiFileIds(built))
    {
        for (const bool data : {false, true})
        {
            const bool may_differ = id == changed && (data ? wave_data : file);
            if (!may_differ && WiiCopies(built_bytes, built, id, data) != WiiCopies(after_bytes, after, id, data))
            {
                return "changes " + std::string(data ? "the wave data of " : "") + "file " + std::to_string(id);
            }
        }
    }

    return {};
}

// A sound's sequence file of the Wii as Replace… reads it, by the rules of `build`, and the item where the sound starts
// in it, if an item starts there.
Source WiiSourceOf(std::span<const uint8_t> bytes, uint32_t start_offset, const ntdlib::SndBuild& build)
{
    Source source{edit::ReadIncoming(bytes, song::Console{.wii = build}), std::nullopt};
    source.start =
        edit::ItemAtOffset(edit::Offsets(source.incoming.sequence, song::Console{.wii = build}), start_offset);

    return source;
}

// Whether the commands that the Wii's sound `info` plays from `source` open tracks that the sound doesn't allocate.
bool Untracked(const Source& source, const ntdlib::rsar::SoundInfo& info)
{
    return (edit::TracksOpened(source.incoming.sequence, *source.start) & ~info.allocate_track_flags) != 0;
}

// What ntdlib's performer plays of sound `sound` of the Wii's `archive` by the rules of `build`, without the places of
// its commands.
song::Sequence WiiPlayed(const ntdlib::rsar::SoundArchive& archive, uint32_t sound, const ntdlib::SndBuild& build)
{
    return Placeless(song::ReadSequence(archive, sound, {}, {}, build));
}

// Whether the sequence file of sound `sound` of the Wii's `archive` has a label at the sound's start.
bool StartsAtLabel(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    const ntdlib::rsar::SoundInfo& info = archive.Sounds()[sound];
    const ntdlib::rsar::SequenceFile file = ntdlib::rsar::SequenceFile::Read(archive.FileData(info.file_id));
    return std::ranges::any_of(file.labels, [&info](const auto& label) { return label.offset == info.start_offset; });
}

// What is wrong when sound `sound` of the Wii's `file` takes its sequence file again from `same`, or nothing.
std::string WiiSameProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                           uint32_t sound, const Source& same)
{
    if (!same.start)
    {
        return "starts where no command of its file starts";
    }

    const std::vector<edit::StartingSound> starts = edit::SoundStarts(file, places, same.incoming.sequence);
    if (std::ranges::find(starts, edit::StartingSound{sound, *same.start}) == starts.end())
    {
        return "isn't offered its start in its file";
    }
    const uint32_t tracks = edit::TracksToAdd(file, places, sound, same.incoming.sequence, *same.start);
    if (edit::ReplaceSequence(file, places, sound, same.incoming.sequence, *same.start, {}, "Check", tracks) != file)
    {
        return "doesn't come back from its file";
    }

    return {};
}

// What is wrong when sound `sound` of the Wii's `file` takes sound `other`'s sequence file, from `source`, and bank, or
// nothing. `built` is the archive that `file` builds, read from `built_bytes`. `before` holds what the performer plays
// of each sound there. A sound whose loop the performer finds a whole number of passes away counts in `totals`. So does
// a sound that plays the notes with another length: its values, such as a volume of 0, determine when its last release
// falls silent.
std::string WiiCrossProblem(const format::File& file, std::span<const std::optional<edit::SoundPlace>> places,
                            const ntdlib::rsar::SoundArchive& built, std::span<const uint8_t> built_bytes,
                            const ntdlib::SndBuild& build, uint32_t sound, uint32_t other, const Source& source,
                            std::map<uint32_t, song::Sequence>& before, WiiTotals& totals)
{
    // The replacement, with the other sound's bank for each sound that plays the new commands. A sound of the Wii plays
    // one bank.
    const uint32_t bank = built.Sounds()[other].bank;
    const std::vector<uint32_t> banks = {bank < built.Banks().size() ? sar::ItemId(sar::kBankItem, bank)
                                                                     : sar::kNoItem};
    const uint32_t tracks = edit::TracksToAdd(file, places, sound, source.incoming.sequence, *source.start);
    const format::File replaced =
        edit::ReplaceSequence(file, places, sound, source.incoming.sequence, *source.start, {}, "Check", tracks, banks);
    std::vector<uint32_t> playing = edit::SharingStart(places, sound);
    playing.push_back(sound);
    hokuto::tools::CheckSaved(replaced, true);
    const std::vector<uint8_t> after_bytes = format::ExportArchive(replaced);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);

    // The sounds that play the new commands play the other sound's notes. The file's other sounds play as before.
    const auto played_before = [&](uint32_t which) -> const song::Sequence&
    {
        auto found = before.find(which);
        if (found == before.end())
        {
            found = before.emplace(which, WiiPlayed(built, which, build)).first;
        }

        return found->second;
    };
    for (const uint32_t player : playing)
    {
        song::Sequence played = WiiPlayed(*after, player, build);
        if (played != played_before(other))
        {
            const uint32_t length = std::exchange(played.length, played_before(other).length);
            if (played == played_before(other))
            {
                totals.other_lengths++;
                continue;
            }
            played.length = length;
            if (!SameUpToLoop(played_before(other), played))
            {
                return "doesn't play " + built.Sounds()[other].name + "'s notes from its file";
            }
            totals.loops_moved++;
        }
    }
    for (const uint32_t sharing : edit::SharingFile(places, sound))
    {
        if (std::ranges::find(playing, sharing) == playing.end() &&
            WiiPlayed(*after, sharing, build) != played_before(sharing))
        {
            return "changes how " + built.Sounds()[sharing].name + " plays";
        }
    }

    // Every copy of the sound's file holds the new commands. The other files keep their bytes in every copy, and the
    // other sounds their starts.
    const uint32_t file_id = built.Sounds()[sound].file_id;
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(after_bytes, *after, file_id, false);
    const std::span<const uint8_t> first = after->FileData(file_id);
    for (const std::vector<uint8_t>& copy : copies)
    {
        if (!std::ranges::equal(copy, first))
        {
            return "leaves a copy of its file with other bytes";
        }
    }
    if (copies.size() != WiiCopies(built_bytes, built, file_id, false).size())
    {
        return "changes the copies of its file";
    }
    std::string problem = OtherWiiFilesProblem(built_bytes, built, after_bytes, *after, file_id, true, false);
    for (std::size_t i = 0; i < built.Sounds().size() && problem.empty(); i++)
    {
        const bool plays = std::ranges::find(playing, static_cast<uint32_t>(i)) != playing.end();
        if (!plays && after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            problem = "moves the start of " + built.Sounds()[i].name;
        }
    }

    return problem;
}

// The banks' files, the wave sound files and the RWARs of the wave data of the Wii's `archive`, each file once, in that
// order. A wave archive's item is its file's ID.
std::vector<FileItem> WiiFileItemsOf(const ntdlib::rsar::SoundArchive& archive)
{
    std::vector<FileItem> items;
    std::set<uint32_t> seen;
    for (uint32_t i = 0; i < archive.Banks().size(); i++)
    {
        const uint32_t id = archive.Banks()[i].file_id;
        if (!archive.FileData(id).empty() && seen.insert(id).second)
        {
            items.push_back({edit::FileKind::kBank, i, id});
        }
    }
    for (const song::WaveSoundSet& set : song::WaveSoundSets(archive))
    {
        if (!set.sounds.empty() && !archive.FileData(set.file_id).empty() && seen.insert(set.file_id).second)
        {
            items.push_back({edit::FileKind::kWaveSoundFile, set.sounds.front(), set.file_id});
        }
    }
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        if (wave_archive.rwar && !archive.WaveData(wave_archive.file_id).empty())
        {
            items.push_back({edit::FileKind::kWaveArchive, wave_archive.file_id, wave_archive.file_id});
        }
    }

    return items;
}

// How the file of `item` of the Wii's `archive` keeps its waves: true in a WAVE block, false in an RWAR, and nothing
// for a wave archive or a file that ntdlib can't read.
std::optional<bool> WaveBlockOf(const ntdlib::rsar::SoundArchive& archive, const FileItem& item)
{
    try
    {
        const std::span<const uint8_t> bytes = archive.FileData(item.file_id);
        if (item.kind == edit::FileKind::kBank)
        {
            return ntdlib::rsar::Bank::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).HasWaveBlock();
        }
        if (item.kind == edit::FileKind::kWaveSoundFile)
        {
            return ntdlib::rsar::WaveSoundFile::Parse(bytes, ntdlib::rvl::snd::kRsarOptions).HasWaveBlock();
        }
    }
    catch (const std::exception&)
    {
        // A file that ntdlib can't read gives no answer.
    }

    return std::nullopt;
}

// `bytes` with zeros up to a multiple of 32 bytes. NW4R's tools pad each file and each wave data that way.
std::vector<uint8_t> PaddedForWii(std::span<const uint8_t> bytes)
{
    std::vector<uint8_t> padded(bytes.begin(), bytes.end());
    padded.resize((padded.size() + 31) / 32 * 32, 0);

    return padded;
}

// What is wrong when the file of `item` of the Wii's `file` takes `bytes` through Replace…'s edit, or nothing. `built`
// is the archive that `file` builds, read from `built_bytes`. With `same`, the bytes are the file's, and the Hokuto
// file has to come back unchanged. Otherwise every copy of the file, or of its wave data for a wave archive, has to
// hold the new bytes, and the other files, the other part of the file and the sounds' starts have to stay. A refusal
// counts in `totals` by its reason.
std::string WiiFileProblem(const format::File& file, const ntdlib::rsar::SoundArchive& built,
                           std::span<const uint8_t> built_bytes, const FileItem& item, std::span<const uint8_t> bytes,
                           bool same, WiiTotals& totals)
{
    // The file as the Replace window reads it, and what it changes.
    const edit::IncomingFile incoming = edit::ReadWiiIncomingFile(bytes, item.kind);
    edit::ChangesOf(file, built, item.item, incoming);
    std::optional<format::File> replaced;
    try
    {
        replaced = edit::ReplaceWiiFile(file, built, item.item, incoming);
    }
    catch (const std::invalid_argument& error)
    {
        totals.refused[error.what()]++;
        return {};
    }
    if (same)
    {
        return *replaced == file ? std::string() : "doesn't come back from its file";
    }

    // The copies take the new bytes, padded as the tools pad them.
    hokuto::tools::CheckSaved(*replaced, true);
    const std::vector<uint8_t> after_bytes = format::ExportArchive(*replaced);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const bool wave_data = item.kind == edit::FileKind::kWaveArchive;
    const std::vector<std::vector<uint8_t>> copies = WiiCopies(after_bytes, *after, item.file_id, wave_data);
    const std::vector<uint8_t> padded = PaddedForWii(incoming.bytes);
    if (copies.size() != WiiCopies(built_bytes, built, item.file_id, wave_data).size())
    {
        return "changes the number of copies";
    }
    for (const std::vector<uint8_t>& copy : copies)
    {
        if (copy != padded)
        {
            return "leaves a copy without the new bytes";
        }
    }
    totals.copies += copies.size();
    std::string problem =
        OtherWiiFilesProblem(built_bytes, built, after_bytes, *after, item.file_id, !wave_data, wave_data);
    for (std::size_t i = 0; i < built.Sounds().size() && problem.empty(); i++)
    {
        if (after->Sounds()[i].start_offset != built.Sounds()[i].start_offset)
        {
            problem = "moves the start of " + built.Sounds()[i].name;
        }
    }

    return problem;
}

// Each file of a bank, a wave sound file and a wave archive of the Wii's `file` takes its bytes again, then the next
// file of its kind that keeps its waves in the same way (WiiFileProblem).
void CheckWiiFiles(const format::File& file, const ntdlib::rsar::SoundArchive& built,
                   std::span<const uint8_t> built_bytes, const std::string& name, WiiTotals& totals)
{
    const std::vector<FileItem> items = WiiFileItemsOf(built);

    const auto bytes_of = [&built](const FileItem& item)
    {
        return item.kind == edit::FileKind::kWaveArchive ? built.WaveData(item.file_id) : built.FileData(item.file_id);
    };
    for (std::size_t i = 0; i < items.size(); i++)
    {
        const FileItem& item = items[i];
        std::string problem;
        try
        {
            problem = WiiFileProblem(file, built, built_bytes, item, bytes_of(item), true, totals);
            totals.files++;

            // The next file of the same kind, kept in the same way.
            const std::optional<bool> block = WaveBlockOf(built, item);
            std::optional<std::size_t> next;
            for (std::size_t step = 1; step < items.size() && problem.empty() && !next; step++)
            {
                const FileItem& other = items[(i + step) % items.size()];
                if (other.kind != item.kind || other.file_id == item.file_id)
                {
                    continue;
                }
                if (WaveBlockOf(built, other) != block)
                {
                    totals.other_form++;
                    continue;
                }
                next = (i + step) % items.size();
            }
            if (problem.empty() && next)
            {
                problem = WiiFileProblem(file, built, built_bytes, item, bytes_of(items[*next]), false, totals);
                totals.files_crossed++;
            }
            else if (problem.empty())
            {
                totals.without_next++;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            const std::string label = item.kind == edit::FileKind::kWaveArchive
                                          ? "the wave data of file " + std::to_string(item.file_id)
                                          : "file " + std::to_string(item.file_id);
            std::printf("%s: %s %s\n", name.c_str(), label.c_str(), problem.c_str());
            totals.files_failed++;
        }
    }
}

// A refusal of an edit of the Wii's: Replace…, File > Import or the Project menu refuses it with this reason. The Wii's
// checks count a refusal by its reason. Any other exception is a failure, a std::invalid_argument of a read after the
// edit included.
struct Refusal : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// A donor's sound, file or wave that the Wii's checks leave out, with the reason.
struct LeftOut : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// The result of `step`, a step that the program can refuse: an edit, or the read of a file that comes in from a donor.
// Its refusal (std::invalid_argument) goes on as a Refusal.
template <typename Step>
auto Refusable(const Step& step)
{
    try
    {
        return step();
    }
    catch (const std::invalid_argument& error)
    {
        throw Refusal(error.what());
    }
}

// What is wrong when wave `wave` of the wave data of file `id` of the Wii's `file` goes back in its place, or nothing.
// `built` is the archive that `file` builds, and `present` holds the wave data's waves. The wave's file has to give the
// Hokuto file back. Its WAV file from Export… then goes in as 16-bit PCM and as DSP-ADPCM. The new wave has to hold the
// wave's rate, channels and loop, and the other waves their values. In 16-bit PCM, it has to decode to the wave's
// samples. In DSP-ADPCM, its loop may move forward to the next frame. A wave of more than two channels counts apart:
// the WAV file holds two. Throws a Refusal when Replace… refuses the wave.
std::string WiiWaveProblem(const format::File& file, const ntdlib::rsar::SoundArchive& built, uint32_t id,
                           uint32_t wave, const std::vector<ntdlib::rsar::WaveFile>& present, WiiTotals& totals)
{
    const ntdlib::rsar::WaveFile& old = present[wave];
    const std::string label = "wave " + std::to_string(wave);
    const edit::IncomingWave same = edit::ReadIncomingWiiWaveFile(old.Write());
    if (Refusable([&] { return edit::WithIncomingWiiWave(file, built, id, wave, same); }) != file)
    {
        return label + " doesn't come back from its wave file";
    }
    totals.waves++;
    if (old.channels.size() > 2)
    {
        totals.waves_wide++;
        return {};
    }

    const std::vector<uint8_t> wav = song::WaveAsWav(built, id, wave);
    const std::vector<std::vector<int16_t>> samples = edit::DecodedSamples(old, old.loop_end);
    for (const sar::WaveEncoding encoding : {sar::WaveEncoding::kPcm16, sar::WaveEncoding::kDspAdpcm})
    {
        const edit::IncomingWave incoming = edit::ReadIncomingWiiWave(wav, old.version, encoding);
        const format::File replaced =
            Refusable([&] { return edit::WithIncomingWiiWave(file, built, id, wave, incoming); });
        hokuto::tools::CheckSaved(replaced, true);
        const WiiArchive after =
            song::LoadWiiArchive(format::ExportArchive(replaced), "after.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<ntdlib::rsar::WaveFile> waves = edit::WavesOf(*after, id);
        if (waves.size() != present.size())
        {
            return label + " changes the number of waves";
        }
        for (std::size_t j = 0; j < waves.size(); j++)
        {
            if (j != wave && waves[j] != present[j])
            {
                return label + " changes wave " + std::to_string(j);
            }
        }

        // A WAV file's loop holds a sample at least. A wave whose loop holds none comes back without the loop.
        const ntdlib::rsar::WaveFile& now = waves[wave];
        const bool empty_loop = old.loop && old.loop_start >= old.loop_end;
        if (now.sample_rate != old.sample_rate || now.channels.size() != old.channels.size() ||
            now.loop != (old.loop && !empty_loop) || now.version != old.version)
        {
            return label + " comes back from its WAV file with other values: " + std::to_string(old.sample_rate) +
                   " Hz to " + std::to_string(now.sample_rate) + " Hz, " + std::to_string(old.channels.size()) +
                   " channels to " + std::to_string(now.channels.size()) + ", loop " + (old.loop ? "on" : "off") +
                   " to " + (now.loop ? "on" : "off") + ", version " + std::to_string(old.version) + " to " +
                   std::to_string(now.version);
        }
        if (encoding == sar::WaveEncoding::kPcm16)
        {
            if ((!empty_loop && now.loop_start != old.loop_start) || now.loop_end != old.loop_end ||
                edit::DecodedSamples(now, old.loop_end) != samples)
            {
                return label + " decodes otherwise from its WAV file";
            }
            totals.empty_loops += empty_loop ? 1 : 0;
            continue;
        }

        // DSP-ADPCM: a loop starts on a frame of 14 samples.
        const bool forward = now.loop_start != old.loop_start;
        if (now.loop && (now.loop_start < old.loop_start || now.loop_start - old.loop_start >= 14))
        {
            return label + " comes back from its WAV file in DSP-ADPCM with its loop elsewhere";
        }
        totals.loops_forward += now.loop && forward ? 1 : 0;
        if (incoming.signal_to_noise)
        {
            totals.ratios.push_back(*incoming.signal_to_noise);
        }
        else
        {
            totals.exact++;
        }
    }

    return {};
}

// Each wave of each wave data of the Wii's `file` goes back in its place (WiiWaveProblem). `built` is the archive that
// `file` builds. Wave data that Replace… can't read counts in `totals` by its reason.
void CheckWiiWaves(const format::File& file, const ntdlib::rsar::SoundArchive& built, const std::string& name,
                   WiiTotals& totals)
{
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(built))
    {
        std::vector<ntdlib::rsar::WaveFile> present;
        try
        {
            present = edit::WavesOf(built, wave_archive.file_id);
        }
        catch (const std::invalid_argument& error)
        {
            totals.refused[error.what()]++;
            continue;
        }
        std::string problem;
        bool refused = false;
        for (uint32_t wave = 0; wave < present.size() && problem.empty() && !refused; wave++)
        {
            try
            {
                problem = WiiWaveProblem(file, built, wave_archive.file_id, wave, present, totals);
            }
            catch (const Refusal& error)
            {
                // Replace… refuses the wave data, such as wave data past the end of an archive that is cut short.
                totals.refused[error.what()]++;
                refused = true;
            }
            catch (const std::exception& error)
            {
                problem = "wave " + std::to_string(wave) + " fails: " + error.what();
            }
        }
        totals.wave_archives += refused ? 0 : 1;
        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), wave_archive.name.c_str(), problem.c_str());
            totals.wave_archives_failed++;
        }
    }
}

// The Wii's imports. Up to `limit` sounds of `type` of the Wii's `archive` whose files the archive holds, spread over
// the sound table.
std::vector<uint32_t> WiiModels(const ntdlib::rsar::SoundArchive& archive, ntdlib::rsar::SoundType type,
                                std::size_t limit)
{
    std::vector<uint32_t> all;
    for (uint32_t s = 0; s < archive.Sounds().size(); s++)
    {
        const ntdlib::rsar::SoundInfo& info = archive.Sounds()[s];
        if (info.type == type && archive.File(info.file_id))
        {
            all.push_back(s);
        }
    }
    const std::size_t count = std::min(limit, all.size());
    std::vector<uint32_t> chosen;
    for (std::size_t k = 0; k < count; k++)
    {
        chosen.push_back(all[k * all.size() / count]);
    }

    return chosen;
}

// The groups with names of the Wii's `file` that hold file `id`: the groups that the Import window proposes.
std::vector<uint32_t> WiiGroupsHolding(const format::File& file, uint32_t id)
{
    std::vector<uint32_t> holding;
    for (const edit::Group& group : edit::WiiGroupsOf(file))
    {
        if (std::ranges::find(group.files, id) != group.files.end())
        {
            holding.push_back(group.group);
        }
    }

    return holding;
}

// What an import changed in the Wii's archive besides its new items, or nothing. `before` and `after` are the models of
// the archive before and after the import, and `joining` holds the files that may join groups. Each table keeps its
// entries, and each file its bytes and its wave data. A group keeps its files, and takes only files of `joining` after
// them.
std::string WiiOthersProblem(const ntdlib::rsar::ArchiveModel& before, const ntdlib::rsar::ArchiveModel& after,
                             std::span<const uint32_t> joining)
{
    const auto keeps = [](const auto& old, const auto& now)
    {
        return now.size() >= old.size() && std::equal(old.begin(), old.end(), now.begin());
    };

    if (after.version != before.version || after.player_settings != before.player_settings ||
        after.players != before.players)
    {
        return "changes the players or their settings";
    }
    if (!keeps(before.sounds, after.sounds) || !keeps(before.banks, after.banks))
    {
        return "changes another sound or bank";
    }
    if (!keeps(before.files, after.files))
    {
        return "changes another file";
    }
    if (after.groups.size() != before.groups.size())
    {
        return "changes the groups";
    }
    for (std::size_t g = 0; g < before.groups.size(); g++)
    {
        const std::vector<uint32_t>& old = before.groups[g].file_ids;
        const std::vector<uint32_t>& now = after.groups[g].file_ids;
        const auto joins = [&joining](uint32_t id)
        {
            return std::ranges::find(joining, id) != joining.end();
        };
        if (after.groups[g].name != before.groups[g].name || !keeps(old, now) ||
            !std::all_of(now.begin() + static_cast<std::ptrdiff_t>(old.size()), now.end(), joins))
        {
            return "changes group " + before.groups[g].name;
        }
    }

    return {};
}

// The counts of the Wii's imports.
struct WiiImportTotals
{
    std::size_t imports = 0;
    std::size_t failed = 0;
    std::size_t without_start = 0; // models that start where no command of their files starts
    std::size_t untracked = 0;     // models whose commands open tracks that they don't allocate
    std::size_t joins = 0;         // groups that took a new file
    std::size_t sounds = 0;        // new wave sounds
    std::size_t waves = 0;         // new waves
    std::size_t sounding = 0;      // copies of models with new banks whose renders hold sound
    std::map<std::string, std::size_t> refused;
};

// What is wrong when the Wii's `file` takes a copy of sequence sound `model` that plays the model's file from its start
// (`source`), or nothing. `built` is the archive that `file` builds, and `before` is that archive's model. The copy
// joins the groups that load the model. Without `bank`, the copy plays the model's bank and has to play the model's
// notes, read by the rules of `build`. With `bank`, the copy plays that bank: a copy of the model's bank that the file
// holds. The copy then has to play the model's samples for 10 s on ntdlib's model of NW4R SND and AX. With `save`, the
// file has to save and open again in both forms. Throws a Refusal when the import refuses the sound.
std::string WiiNewSoundProblem(const format::File& file, const ntdlib::rsar::SoundArchive& built,
                               const ntdlib::rsar::ArchiveModel& before, const ntdlib::SndBuild& build, uint32_t model,
                               const Source& source, std::optional<uint32_t> bank, bool save, WiiImportTotals& totals)
{
    const ntdlib::rsar::SoundInfo& info = built.Sounds()[model];
    const std::vector<uint32_t> groups = WiiGroupsHolding(file, info.file_id);
    const std::string sound_name = "HOKUTO_CHECK_" + std::to_string(model);
    const std::optional<std::vector<uint32_t>> banks =
        bank ? std::optional(std::vector<uint32_t>{sar::ItemId(sar::kBankItem, *bank)}) : std::nullopt;
    const auto add_sound = [&]
    {
        return edit::AddWiiSequenceSound(file, model, sound_name, source.incoming.sequence, *source.start, {}, banks,
                                         groups);
    };
    const format::File added = Refusable(add_sound);
    totals.imports++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel now = ntdlib::rsar::ReadArchiveModel(after_bytes);

    // The new sound holds the model's values, with the new file, its start and the tracks that its commands open. The
    // groups that load the model hold the new file.
    const auto id = static_cast<uint32_t>(now.files.size() - 1);
    const auto sound = static_cast<uint32_t>(now.sounds.size() - 1);
    ntdlib::rsar::SoundModel expected = now.sounds.at(model);
    expected.name = sound_name;
    expected.file_id = id;
    expected.start_offset = now.sounds.back().start_offset;
    expected.bank = bank.value_or(expected.bank);
    expected.allocate_track_flags = edit::TracksOpened(source.incoming.sequence, *source.start);
    if (sound != before.sounds.size())
    {
        return "adds more than one sound";
    }
    if (now.sounds.back() != expected)
    {
        return "doesn't give the new sound the model's values";
    }
    for (const uint32_t g : groups)
    {
        totals.joins++;
        if (std::ranges::find(now.groups[g].file_ids, id) == now.groups[g].file_ids.end())
        {
            return "leaves the new file out of " + now.groups[g].name;
        }
    }
    std::vector<uint32_t> joining = {id};
    if (bank)
    {
        joining.push_back(now.banks.at(*bank).file_id);
    }
    if (std::string problem = WiiOthersProblem(before, now, joining); !problem.empty())
    {
        return problem;
    }

    // The file's sequences keep their numbers. The new sound plays the model's notes, or its samples with the copy of
    // its bank.
    for (std::size_t i = 0; i < file.sequences.size(); i++)
    {
        if (added.sequences[i] != file.sequences[i])
        {
            return "changes the file's sequence " + std::to_string(i);
        }
    }
    if (!bank && WiiPlayed(*after, sound, build) != WiiPlayed(built, model, build))
    {
        return "doesn't play the model's notes";
    }
    if (bank)
    {
        const std::atomic<bool> kCancel{false};
        const auto render = [&build, &kCancel](const WiiArchive& archive, uint32_t which)
        {
            const auto sequence =
                std::make_shared<const song::Sequence>(song::ReadSequence(*archive, which, {}, {}, build));
            return song::RenderDry(archive, sequence, 10.0, kCancel).samples;
        };

        const WiiArchive original =
            song::LoadWiiArchive(format::ExportArchive(file), "before.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<float> samples = render(original, model);
        if (render(after, sound) != samples)
        {
            return "doesn't play the model's samples with the copy of its bank";
        }
        totals.sounding += std::ranges::any_of(samples, [](float sample) { return sample != 0.0f; }) ? 1 : 0;
    }
    if (save && !hokuto::tools::SavesAndOpens(added, true))
    {
        return "doesn't save and open again";
    }

    return {};
}

// Checks File > Import > Sequence…, Bank…, Wave Sound File…, Wave… and Audio File… on the Wii's `file` by `mode`.
// `file` builds `built` from `built_bytes`. Prints what fails and a summary. Returns the exit code: 1 when something
// fails.
int CheckWiiImports(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                    const ntdlib::SndBuild& build, const std::string& name, const std::string& mode)
{
    // An archive that the tools didn't lay out takes no new item.
    ntdlib::rsar::ArchiveModel before;
    try
    {
        before = ntdlib::rsar::ReadArchiveModel(built_bytes);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: takes no new item: %s\n", name.c_str(), error.what());
        return 0;
    }

    WiiImportTotals totals;
    const auto fail = [&name, &totals](const std::string& item, const std::string& problem)
    {
        std::printf("%s: %s %s\n", name.c_str(), item.c_str(), problem.c_str());
        totals.failed++;
    };
    const auto attempt = [&totals, &fail](const std::string& item, const auto& check)
    {
        try
        {
            if (const std::string problem = check(); !problem.empty())
            {
                fail(item, problem);
            }
        }
        catch (const Refusal& error)
        {
            totals.refused[error.what()]++;
        }
        catch (const std::exception& error)
        {
            fail(item, std::string("fails: ") + error.what());
        }
    };

    const auto source_of = [&built, &build, &totals](uint32_t model) -> std::optional<Source>
    {
        const ntdlib::rsar::SoundInfo& info = built->Sounds()[model];
        Source source = WiiSourceOf(built->FileData(info.file_id), info.start_offset, build);
        if (!source.start)
        {
            totals.without_start++;
            return std::nullopt;
        }
        if (Untracked(source, info))
        {
            totals.untracked++;
            return std::nullopt;
        }

        return source;
    };

    if (mode == "--new")
    {
        // Up to 32 sequence sounds serve as models. Each new sound plays its model's file from the model's start.
        bool first = true;
        for (const uint32_t model : WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 32))
        {
            const std::optional<Source> source = source_of(model);
            if (source)
            {
                const auto check_sound = [&]
                {
                    return WiiNewSoundProblem(file, *built, before, build, model, *source, {}, first, totals);
                };
                attempt(built->Sounds()[model].name, check_sound);
                first = false;
            }
        }
    }
    else if (mode == "--banks")
    {
        // Up to 8 sequence sounds serve as models. Each one's bank goes in again with its wave data, and a copy of the
        // model plays the new bank.
        for (const uint32_t model : WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 8))
        {
            const ntdlib::rsar::SoundInfo& info = built->Sounds()[model];
            const std::optional<Source> source = source_of(model);
            const std::optional<ntdlib::rsar::FileData> data =
                info.bank < built->Banks().size() ? built->File(built->Banks()[info.bank].file_id) : std::nullopt;
            if (!source || !data)
            {
                continue;
            }
            attempt(built->Sounds()[model].name,
                    [&]
                    {
                        const uint32_t bank_file = built->Banks()[info.bank].file_id;
                        const auto add_bank = [&]
                        {
                            return edit::AddWiiBank(file, "HOKUTO_CHECK_BANK", data->file, data->wave_data,
                                                    WiiGroupsHolding(file, bank_file));
                        };
                        const format::File with_bank = Refusable(add_bank);
                        hokuto::tools::CheckSaved(with_bank, true);
                        const ntdlib::rsar::ArchiveModel banked =
                            ntdlib::rsar::ReadArchiveModel(format::ExportArchive(with_bank));
                        const auto id = static_cast<uint32_t>(before.files.size());
                        if (banked.banks.size() != before.banks.size() + 1 || banked.banks.back().file_id != id ||
                            banked.files.at(id).bytes != before.files.at(bank_file).bytes ||
                            banked.files.at(id).wave_data != before.files.at(bank_file).wave_data)
                        {
                            return std::string("doesn't hold the copy of its bank");
                        }
                        if (std::string problem = WiiOthersProblem(before, banked, std::span(&id, 1)); !problem.empty())
                        {
                            return problem;
                        }
                        const WiiArchive with = song::LoadWiiArchive(format::ExportArchive(with_bank), "bank.brsar",
                                                                     ntdlib::rvl::snd::kRsarOptions);
                        return WiiNewSoundProblem(with_bank, *with, banked, build, model, *source,
                                                  static_cast<uint32_t>(before.banks.size()), false, totals);
                    });
        }
    }
    else if (mode == "--wave-sounds")
    {
        // Up to 8 wave sounds' files go in again, once with the wave sound as the model, and once with the archive's
        // first sequence sound. The new sound in the wave sound's place in the file plays the wave sound's wave.
        const std::vector<uint32_t> sequences = WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 1);
        for (const uint32_t model : WiiModels(*built, ntdlib::rsar::SoundType::kWaveSound, 8))
        {
            const ntdlib::rsar::SoundInfo& info = built->Sounds()[model];
            const std::optional<ntdlib::rsar::FileData> data = built->File(info.file_id);
            std::vector<uint32_t> models = {model};
            models.insert(models.end(), sequences.begin(), sequences.end());
            for (const uint32_t copied : models)
            {
                attempt(
                    info.name,
                    [&]
                    {
                        const std::vector<uint32_t> groups = WiiGroupsHolding(file, built->Sounds()[copied].file_id);
                        const auto add_wave_sounds = [&]
                        {
                            return edit::AddWiiWaveSounds(file, copied, "HOKUTO_CHECK_WSD", data->file, data->wave_data,
                                                          groups);
                        };
                        const format::File added = Refusable(add_wave_sounds);
                        hokuto::tools::CheckSaved(added, true);
                        totals.imports++;
                        const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
                        const WiiArchive after =
                            song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
                        const ntdlib::rsar::ArchiveModel now = ntdlib::rsar::ReadArchiveModel(after_bytes);
                        const auto id = static_cast<uint32_t>(before.files.size());
                        const std::size_t count =
                            ntdlib::rsar::WaveSoundFile::Parse(data->file, ntdlib::rvl::snd::kRsarOptions)
                                .Sounds()
                                .size();
                        if (now.sounds.size() != before.sounds.size() + count)
                        {
                            return std::string("adds another number of sounds");
                        }
                        totals.sounds += count;
                        totals.joins += groups.size();
                        for (std::size_t k = 0; k < count; k++)
                        {
                            const ntdlib::rsar::SoundModel& sound = now.sounds[before.sounds.size() + k];
                            const ntdlib::rsar::SoundModel& like = before.sounds[copied];
                            if (sound.type != ntdlib::rsar::SoundType::kWaveSound || sound.file_id != id ||
                                sound.wave_sound_index != static_cast<int32_t>(k) || sound.volume != like.volume ||
                                sound.player_id != like.player_id || sound.player_priority != like.player_priority ||
                                sound.channel_priority != like.channel_priority ||
                                sound.settings_3d != like.settings_3d)
                            {
                                return "doesn't give new sound " + std::to_string(k) + " the model's values";
                            }
                        }
                        for (const uint32_t g : groups)
                        {
                            if (std::ranges::find(now.groups[g].file_ids, id) == now.groups[g].file_ids.end())
                            {
                                return "leaves the new file out of " + now.groups[g].name;
                            }
                        }
                        if (std::string problem = WiiOthersProblem(before, now, std::span(&id, 1)); !problem.empty())
                        {
                            return problem;
                        }
                        const auto placed =
                            static_cast<uint32_t>(before.sounds.size()) + static_cast<uint32_t>(info.sub_index);
                        if (!SamePcm(song::DecodeWaveSound(*after, placed), song::DecodeWaveSound(*built, model)))
                        {
                            return std::string("doesn't play the wave sound's wave");
                        }

                        return std::string();
                    });
            }
        }
    }
    else
    {
        // The first wave of each wave data goes in again after the waves, from its wave file and from its WAV file.
        for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*built))
        {
            attempt(
                wave_archive.name,
                [&]
                {
                    const std::vector<ntdlib::rsar::WaveFile> present =
                        Refusable([&] { return edit::WavesOf(*built, wave_archive.file_id); });
                    if (present.empty())
                    {
                        return std::string();
                    }
                    const ntdlib::rsar::WaveFile& old = present.front();
                    for (const bool wav : {false, true})
                    {
                        const edit::IncomingWave incoming =
                            wav ? edit::ReadIncomingWiiWave(song::WaveAsWav(*built, wave_archive.file_id, 0),
                                                            old.version, sar::WaveEncoding::kPcm16)
                                : edit::ReadIncomingWiiWaveFile(old.Write());
                        const auto add_wave = [&]
                        {
                            return edit::WithIncomingWiiWave(file, *built, wave_archive.file_id, std::nullopt,
                                                             incoming);
                        };
                        const format::File added = Refusable(add_wave);
                        hokuto::tools::CheckSaved(added, true);
                        totals.imports++;
                        const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
                        const WiiArchive after =
                            song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
                        const std::vector<ntdlib::rsar::WaveFile> waves = edit::WavesOf(*after, wave_archive.file_id);
                        if (waves.size() != present.size() + 1 ||
                            !std::equal(present.begin(), present.end(), waves.begin()))
                        {
                            return std::string("doesn't keep the waves before the new one");
                        }
                        const ntdlib::rsar::WaveFile& now = waves.back();
                        const bool empty_loop = old.loop && old.loop_start >= old.loop_end;
                        if (!wav && now != old)
                        {
                            return std::string("doesn't take the wave from its wave file");
                        }
                        if (wav && (now.sample_rate != old.sample_rate || now.channels.size() != old.channels.size() ||
                                    now.loop != (old.loop && !empty_loop) ||
                                    (!empty_loop && now.loop_start != old.loop_start) || now.loop_end != old.loop_end ||
                                    now.version != old.version ||
                                    edit::DecodedSamples(now, old.loop_end) != edit::DecodedSamples(old, old.loop_end)))
                        {
                            return std::string("doesn't take the wave from its WAV file");
                        }
                        if (std::string problem = OtherWiiFilesProblem(built_bytes, *built, after_bytes, *after,
                                                                       wave_archive.file_id, true, true);
                            !problem.empty())
                        {
                            return problem;
                        }
                        totals.waves++;
                    }

                    return std::string();
                });
        }
    }

    std::printf(
        "%s: %zu imports of the Wii (%s). %zu fail. %zu models start where no command of their files starts, "
        "and %zu open tracks that they don't allocate. %zu groups took a new file. %zu new wave sounds and %zu "
        "new waves. %zu renders of copies with a new bank hold sound.\n",
        name.c_str(), totals.imports, mode.c_str(), totals.failed, totals.without_start, totals.untracked, totals.joins,
        totals.sounds, totals.waves, totals.sounding);
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }

    return totals.failed == 0 ? 0 : 1;
}

// The imports and replacements of a later console's files in an archive of the Wii. The donor is an archive of the 3DS,
// the Wii U or the Switch.

// The Wii's file after an edit, the archive that it builds, and the archive's model.
struct WiiState
{
    format::File file;
    std::vector<uint8_t> bytes;
    WiiArchive archive;
    ntdlib::rsar::ArchiveModel model;
};

// `file` with the archive that it builds and the archive's model.
WiiState WiiStateOf(format::File file)
{
    WiiState state{std::move(file), {}, {}, {}};
    hokuto::tools::CheckSaved(state.file, true);
    state.bytes = format::ExportArchive(state.file);
    state.archive = song::LoadWiiArchive(state.bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    state.model = ntdlib::rsar::ReadArchiveModel(state.bytes);

    return state;
}

// What the checks with a donor counted.
struct WiiDonorTotals
{
    std::size_t imports = 0;
    std::size_t failed = 0;
    std::size_t sounds = 0;                     // sequence sounds that came in with their banks
    std::size_t same_samples = 0;               // of them, those that play the samples of Another Console's conversion
    std::size_t sounding = 0;                   // of them, those whose renders hold sound
    std::size_t wave_archives = 0;              // wave archives that came in as banks that hold their waves
    std::size_t waves = 0;                      // waves of those wave archives, and new waves
    std::size_t banks = 0;                      // banks that came in, or took a bank's place
    std::size_t wave_sounds = 0;                // new wave sounds
    std::size_t files = 0;                      // files of the Wii archive that took a donor's file in their place
    std::size_t copies = 0;                     // copies of those files in the groups
    std::size_t unlabelled = 0;                 // donor's sounds that start where their files have no label
    std::size_t without_bank = 0;               // donor's sounds without a bank in their first slot
    std::size_t lacked = 0;                     // donor's items whose files the donor lacks
    std::size_t defaults = 0;                   // conversions that take the default pan mode and curve before 1.2
    std::size_t several_slots = 0;              // donor's sounds whose conversions take the banks of several slots
    std::size_t trimmed = 0;                    // donor's sounds whose conversions keep only the selected instruments
    std::map<std::string, std::size_t> refused; // refusals, by their reasons
};

// Whether `wii`, a wave of the Wii that a wave of a later console became, keeps `later`'s rate, its loop and the
// samples of its first two channels.
bool KeepsWave(const ntdlib::rsar::WaveFile& wii, const sar::WaveFile& later)
{
    const std::size_t channels = std::min<std::size_t>(later.channels.size(), 2);
    if (wii.sample_rate != later.sample_rate || wii.loop != later.loop || wii.loop_end != later.loop_end ||
        (later.loop && wii.loop_start != later.loop_start) || wii.channels.size() != channels)
    {
        return false;
    }
    std::vector<std::vector<int16_t>> samples = edit::DecodedSamples(later, later.loop_end);
    samples.resize(channels);

    return edit::DecodedSamples(wii, wii.loop_end) == samples;
}

// Whether `wii`, the samples of a wave sound of the Wii that a wave sound of a later console became, keeps `later`'s
// rate, its loop and its first two channels.
bool KeepsPcm(const sar::Pcm& wii, sar::Pcm later)
{
    later.channels.resize(std::min<std::size_t>(later.channels.size(), 2));

    return SamePcm(wii, later);
}

// The bytes of `item` of the donor, or nothing when the donor lacks its file.
std::optional<std::vector<uint8_t>> DonorBytes(const sar::SoundArchive& donor, const song::Item& item)
{
    try
    {
        const std::span<const uint8_t> bytes = song::ItemFile(donor, item);
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
    }
    catch (const std::runtime_error&)
    {
        return std::nullopt;
    }
}

// Wave archive `id` of the donor, an item ID, in the Wii's `state` as a bank without instruments that holds its waves:
// File > Import > Wave Archive… on the Wii. The waves have to keep the donor's samples. Gives the new bank's file ID.
// Throws a Refusal for a wave archive that the Wii can't take, a LeftOut for a wave archive that the donor lacks, and
// std::runtime_error with a problem.
uint32_t ImportDonorWaves(WiiState& state, const sar::SoundArchive& donor, uint32_t id, WiiDonorTotals& totals)
{
    const std::optional<std::vector<uint8_t>> bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kWaveArchive, .index = id & 0xFFFFFF});
    if (!bytes)
    {
        throw LeftOut("lacks wave archive " + std::to_string(id & 0xFFFFFF));
    }
    const edit::IncomingFile incoming =
        Refusable([&] { return edit::ReadWiiIncomingFile(*bytes, edit::FileKind::kWaveArchive); });
    std::vector<ntdlib::rsar::WaveFile> waves;
    for (const std::vector<uint8_t>& wave : ntdlib::rsar::WaveArchiveFile::Read(incoming.bytes).waves)
    {
        waves.push_back(ntdlib::rsar::WaveFile::Read(wave));
    }
    const sar::WaveArchiveFile later = sar::WaveArchiveFile::Read(*bytes);
    if (waves.size() != later.waves.size())
    {
        throw std::runtime_error("wave archive " + std::to_string(id & 0xFFFFFF) + " keeps another number of waves");
    }
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        if (!KeepsWave(waves[i], sar::WaveFile::Read(later.waves[i])))
        {
            throw std::runtime_error("wave " + std::to_string(i) + " of wave archive " + std::to_string(id & 0xFFFFFF) +
                                     " changes");
        }
    }

    const ntdlib::rsar::BankFile::Files files = Refusable([&] { return edit::EmptyBank(*state.archive, waves); });
    const std::size_t before = state.model.files.size();
    const auto add_bank = [&]
    {
        return edit::AddWiiBank(state.file, "HOKUTO_CHECK_WARC_" + std::to_string(id & 0xFFFFFF), files.file,
                                files.wave_data, {});
    };
    state = WiiStateOf(Refusable(add_bank));
    totals.imports++;
    totals.wave_archives++;
    totals.waves += waves.size();
    const uint32_t file_id = state.archive->Banks().back().file_id;
    if (state.model.files.size() != before + 1 || file_id != before || edit::WavesOf(*state.archive, file_id) != waves)
    {
        throw std::runtime_error("wave archive " + std::to_string(id & 0xFFFFFF) + " doesn't come in with its waves");
    }

    return file_id;
}

// The wave archives that `bytes`, a bank file or a wave sound file of the donor, names, in the Wii's `state` as banks
// that hold their waves (ImportDonorWaves). Gives the file of each by the wave archive's item ID.
std::map<uint32_t, uint32_t> ImportNamedWaves(WiiState& state, const sar::SoundArchive& donor,
                                              std::span<const uint8_t> bytes, WiiDonorTotals& totals)
{
    std::map<uint32_t, uint32_t> map;
    const std::vector<edit::NamedWaveArchive> names =
        Refusable([&] { return edit::WaveArchivesNamed(bytes, donor.ReadOptions().flavor); });
    for (const edit::NamedWaveArchive& named : names)
    {
        map[named.id] = ImportDonorWaves(state, donor, named.id, totals);
    }

    return map;
}

// The reasons why the check leaves out a donor's sound whose conversion File > Import can't match: the import brings in
// the bank of one slot, whole.
constexpr std::string_view kSeveralSlots = "plays the banks of several slots";
constexpr std::string_view kTrimmed = "keeps only the instruments that it selects";

// What is wrong when sequence sound `sound` of the donor comes into the Wii's `file` with the bank of its first slot
// and the wave archives that the bank names, or nothing. The new sound copies the values of the archive's first
// sequence sound `model`, and then takes the volume, the pan mode and the pan curve of the sound that Another Console
// makes for `build` (edit::ForWii). It has to play that sound's samples for `seconds` by the rules of `build`. An
// archive before version 1.2 holds no pan mode and no pan curve. NW4R SND then pans in balance mode, with the
// square-root curve (ntdlib::rsar::SoundInfo). A conversion with other values takes those for the reference, and
// `totals` counts it. A conversion that takes the banks of several slots, or keeps only the instruments that the sound
// selects, has no import to match. Throws a Refusal for an import that Hokuto refuses, and a LeftOut for a sound that
// the check leaves out.
std::string DonorSoundProblem(const format::File& file, const sar::SoundArchive& donor, const format::File& donor_file,
                              uint32_t sound, uint32_t model, const ntdlib::SndBuild& build, double seconds,
                              WiiDonorTotals& totals)
{
    const sar::SoundInfo& info = donor.Sounds()[sound];
    if (info.banks.empty() || info.banks[0] == sar::kNoItem)
    {
        throw LeftOut("without a bank in its first slot");
    }
    const std::optional<std::vector<uint8_t>> bank_bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kBank, .index = info.banks[0] & 0xFFFFFF});
    const std::optional<std::vector<uint8_t>> sequence_bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kSequence, .index = sound});
    if (!bank_bytes || !sequence_bytes)
    {
        throw LeftOut("lacks its files");
    }

    // The sound's start: a label of its file, or the file's first command when the file has no label.
    const edit::Incoming read = Refusable([&] { return edit::ReadIncoming(*sequence_bytes); });
    const std::optional<uint32_t> item =
        edit::ItemAtOffset(edit::Offsets(read.sequence, song::Console{.flavor = read.flavor}), info.start_offset);
    std::optional<std::string> label;
    for (const ntdlib::commands::Label& named : read.sequence.labels)
    {
        label = item && named.item == *item ? std::optional(named.name) : label;
    }
    if (!label && !(read.sequence.labels.empty() && info.start_offset == 0))
    {
        throw LeftOut("starts where its file has no label");
    }

    // The reference: the sound that Another Console makes for the archive's build.
    edit::OtherConsoleSong other = Refusable([&] { return edit::ForWii(donor_file, sound, build); });
    if (!other.slot_programs.empty())
    {
        throw LeftOut(std::string(kSeveralSlots));
    }
    const auto kept_only = [](const std::string& warning)
    {
        return warning.starts_with("The bank goes in with only the instruments");
    };
    if (std::ranges::any_of(other.warnings, kept_only))
    {
        throw LeftOut(std::string(kTrimmed));
    }
    const ntdlib::rsar::SoundModel converted =
        ntdlib::rsar::ReadArchiveModel(format::ExportArchive(other.file)).sounds.at(0);

    // The wave archives, the bank and the sound.
    WiiState state = WiiStateOf(file);
    const std::map<uint32_t, uint32_t> map = ImportNamedWaves(state, donor, *bank_bytes, totals);
    const sar::Flavor flavor = donor.ReadOptions().flavor;
    const auto convert_bank = [&]
    {
        return edit::WiiFileOf(edit::ReadIncomingFile(*bank_bytes, edit::FileKind::kBank, flavor), flavor,
                               *state.archive, map, edit::NewWiiFileVersion(*state.archive, edit::FileKind::kBank));
    };
    const edit::IncomingFile wii_bank = Refusable(convert_bank);
    const auto add_bank = [&]
    {
        return edit::AddWiiBank(state.file, "HOKUTO_CHECK_BANK", wii_bank.bytes, wii_bank.wave_data, {});
    };
    state = WiiStateOf(Refusable(add_bank));
    totals.imports++;
    totals.banks++;
    const auto bank = static_cast<uint32_t>(state.archive->Banks().size() - 1);
    edit::Incoming incoming =
        Refusable([&] { return edit::ReadIncoming(*sequence_bytes, song::Console{.wii = build}); });
    uint32_t start = 0;
    for (const ntdlib::commands::Label& named : incoming.sequence.labels)
    {
        start = label && named.name == *label ? named.item : start;
    }
    start = Refusable([&] { return edit::WithLaterGlobals(incoming.sequence, start).first; });
    const std::size_t sounds = state.model.sounds.size();
    const auto add_sound = [&]
    {
        return edit::AddWiiSequenceSound(state.file, model, "HOKUTO_CHECK_SEQ", incoming.sequence, start, {},
                                         std::vector<uint32_t>{sar::ItemId(sar::kBankItem, bank)}, {});
    };
    state = WiiStateOf(Refusable(add_sound));
    totals.imports++;
    const auto added = static_cast<uint32_t>(sounds);
    if (state.model.sounds.size() != sounds + 1 || state.model.sounds.back().bank != bank)
    {
        return "doesn't add the sound with its bank";
    }

    // The conversion's values. Before version 1.2, the conversion takes the balance mode and the square-root curve.
    format::File valued = edit::ChangeSound(state.file, added, {edit::SoundValue::kVolume, 0, converted.volume});
    if (edit::HeldValuesOf(valued).pan)
    {
        valued = edit::ChangeSound(valued, added, {edit::SoundValue::kPanMode, 0, converted.pan_mode});
        valued = edit::ChangeSound(valued, added, {edit::SoundValue::kPanCurve, 0, converted.pan_curve});
    }
    else if (converted.pan_mode != 1 || converted.pan_curve != 0)
    {
        other.file = edit::ChangeSound(other.file, 0, {edit::SoundValue::kPanMode, 0, 1});
        other.file = edit::ChangeSound(other.file, 0, {edit::SoundValue::kPanCurve, 0, 0});
        totals.defaults++;
    }
    state = WiiStateOf(std::move(valued));
    totals.sounds++;
    const WiiArchive reference =
        song::LoadWiiArchive(format::ExportArchive(other.file), "other.brsar", ntdlib::rvl::snd::kRsarOptions);

    // Both play by the rules of the archive's build.
    const std::atomic<bool> kCancel{false};
    const auto render = [&build, &kCancel, seconds](const WiiArchive& archive, uint32_t which)
    {
        const auto sequence =
            std::make_shared<const song::Sequence>(song::ReadSequence(*archive, which, {}, {}, build));
        return song::RenderDry(archive, sequence, seconds, kCancel).samples;
    };
    const std::vector<float> expected = render(reference, 0);
    const std::vector<float> samples = render(state.archive, added);
    totals.sounding += std::ranges::any_of(expected, [](float sample) { return sample != 0.0f; }) ? 1 : 0;
    if (samples != expected)
    {
        return "doesn't play the samples of Another Console's conversion";
    }
    totals.same_samples++;

    return {};
}

// What is wrong when wave sound file `set` of the donor comes into the Wii's `file` with the wave archives that its
// waves name, or nothing. The new wave sounds copy the values of the Wii's sound `model`. Each has to play the wave of
// the donor's wave sound at the same rate: the first two channels of its samples, with its loop. Throws a Refusal for
// an import that Hokuto refuses, and a LeftOut for a file that the check leaves out.
std::string DonorWaveSoundsProblem(const format::File& file, const sar::SoundArchive& donor,
                                   const song::WaveSoundSet& set, uint32_t model, WiiDonorTotals& totals)
{
    const std::span<const uint8_t> data = donor.FileData(set.file_id);
    if (data.empty())
    {
        throw LeftOut("lacks its file");
    }
    const std::vector<uint8_t> bytes(data.begin(), data.end());
    WiiState state = WiiStateOf(file);
    const std::map<uint32_t, uint32_t> map = ImportNamedWaves(state, donor, bytes, totals);
    const sar::Flavor flavor = donor.ReadOptions().flavor;
    const auto convert_file = [&]
    {
        return edit::WiiFileOf(edit::ReadIncomingFile(bytes, edit::FileKind::kWaveSoundFile, flavor), flavor,
                               *state.archive, map,
                               edit::NewWiiFileVersion(*state.archive, edit::FileKind::kWaveSoundFile));
    };
    const edit::IncomingFile wii = Refusable(convert_file);
    const std::size_t sounds = state.model.sounds.size();
    const auto add_wave_sounds = [&]
    {
        return edit::AddWiiWaveSounds(state.file, model, "HOKUTO_CHECK_WSD", wii.bytes, wii.wave_data, {});
    };
    state = WiiStateOf(Refusable(add_wave_sounds));
    totals.imports++;
    for (const uint32_t sound : set.sounds)
    {
        const auto placed = static_cast<uint32_t>(sounds + donor.Sounds()[sound].wave_sound_index);
        if (placed >= state.model.sounds.size() ||
            state.model.sounds[placed].type != ntdlib::rsar::SoundType::kWaveSound)
        {
            return "doesn't add wave sound " + donor.Sounds()[sound].name;
        }
        if (!KeepsPcm(song::DecodeWaveSound(*state.archive, placed), song::DecodeWaveSound(donor, sound)))
        {
            return "doesn't play the wave of " + donor.Sounds()[sound].name;
        }
        totals.wave_sounds++;
    }

    return {};
}

// What is wrong when the first wave of the donor's wave archive `index` comes into the Wii's `file` after the waves of
// the wave data of file `id`, and in the place of its first wave, or nothing. The new wave has to keep the donor's
// rate, loop and samples, and the other waves their values. Throws a Refusal for a wave that Hokuto refuses, and a
// LeftOut for a wave that the check leaves out.
std::string DonorWaveProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             const sar::SoundArchive& donor, uint32_t index, uint32_t id, WiiDonorTotals& totals)
{
    const std::optional<std::vector<uint8_t>> bytes =
        DonorBytes(donor, {.kind = song::ItemKind::kWave, .index = index, .wave = 0});
    if (!bytes)
    {
        throw LeftOut("lacks its file");
    }
    const sar::WaveFile later = sar::WaveFile::Read(*bytes);
    const edit::IncomingWave incoming = Refusable([&] { return edit::ReadIncomingWiiWaveFile(*bytes); });
    if (!incoming.converted || incoming.converted->flavor != donor.ReadOptions().flavor)
    {
        return "doesn't go in converted";
    }
    const std::vector<ntdlib::rsar::WaveFile> present = Refusable([&] { return edit::WavesOf(*built, id); });
    for (const std::optional<uint32_t> wave : {std::optional<uint32_t>(), std::optional<uint32_t>(0)})
    {
        if (wave && present.empty())
        {
            continue;
        }

        const format::File replaced =
            Refusable([&] { return edit::WithIncomingWiiWave(file, *built, id, wave, incoming); });
        hokuto::tools::CheckSaved(replaced, true);
        totals.imports++;
        const std::vector<uint8_t> after_bytes = format::ExportArchive(replaced);
        const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
        const std::vector<ntdlib::rsar::WaveFile> waves = edit::WavesOf(*after, id);
        const std::size_t at = wave ? 0 : present.size();
        if (waves.size() != present.size() + (wave ? 0 : 1) || !KeepsWave(waves[at], later))
        {
            return wave ? "doesn't take the wave in the place of the first" : "doesn't take the wave after the waves";
        }
        for (std::size_t i = 0; i < present.size(); i++)
        {
            if (i != at && waves[i] != present[i])
            {
                return "changes wave " + std::to_string(i);
            }
        }
        if (std::string problem = OtherWiiFilesProblem(built_bytes, *built, after_bytes, *after, id, true, true);
            !problem.empty())
        {
            return problem;
        }
        totals.waves++;
    }

    return {};
}

// What is wrong when the file of the Wii's bank or wave sound `item`, of file `id`, takes `bytes`, a file of `kind` of
// the donor, in its place, or nothing. Each wave archive that the donor's file names maps to the present file's wave
// data. That is the Replace window's first proposal. Every copy of the file and of its wave data has to take the new
// bytes, and every other file has to keep its bytes. Throws a Refusal for a replacement that Hokuto refuses.
std::string DonorFileProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             edit::FileKind kind, uint32_t item, uint32_t id, std::span<const uint8_t> bytes,
                             sar::Flavor flavor, WiiDonorTotals& totals)
{
    std::map<uint32_t, uint32_t> map;
    for (const edit::NamedWaveArchive& named : Refusable([&] { return edit::WaveArchivesNamed(bytes, flavor); }))
    {
        map[named.id] = id;
    }
    const auto convert_file = [&]
    {
        return edit::WiiFileOf(edit::ReadIncomingFile(bytes, kind, flavor), flavor, *built, map,
                               edit::PresentWiiVersion(*built, kind, item));
    };
    const edit::IncomingFile wii = Refusable(convert_file);
    const format::File replaced = Refusable([&] { return edit::ReplaceWiiFile(file, *built, item, wii); });
    hokuto::tools::CheckSaved(replaced, true);
    totals.imports++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(replaced);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    for (const bool wave_data : {false, true})
    {
        const std::vector<std::vector<uint8_t>> copies = WiiCopies(after_bytes, *after, id, wave_data);
        if (copies.size() != WiiCopies(built_bytes, *built, id, wave_data).size())
        {
            return "changes the number of copies";
        }
        for (const std::vector<uint8_t>& copy : copies)
        {
            if (copy != PaddedForWii(wave_data ? wii.wave_data : wii.bytes))
            {
                return wave_data ? "leaves a copy with other wave data" : "leaves a copy with other bytes";
            }

            totals.copies += wave_data ? 0 : 1;
        }
    }
    if (std::string problem = OtherWiiFilesProblem(built_bytes, *built, after_bytes, *after, id, true, true);
        !problem.empty())
    {
        return problem;
    }
    totals.files++;
    totals.banks += kind == edit::FileKind::kBank ? 1 : 0;

    return {};
}

// Checks the Wii's imports of the files of `donor_path`, an archive of the 3DS, the Wii U or the Switch, in the Wii's
// `file` by `mode`. `file` builds `built` from `built_bytes`. Prints what fails and a summary. Returns the exit code: 1
// when something fails, and 2 when the donor can't be read.
int CheckWiiDonor(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                  const ntdlib::SndBuild& build, const std::string& name, const std::string& mode,
                  const std::filesystem::path& donor_path)
{
    // The donor and its file.
    const std::string donor_name = xsflib::Utf8(donor_path.filename());
    Archive donor;
    format::File donor_file;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(donor_path, bytes))
        {
            throw std::runtime_error("can't be read");
        }
        donor = song::OpenArchive(donor_path);
        donor_file = format::ImportArchive(bytes, donor->ReadOptions().flavor, donor->GroupFiles(),
                                           song::ReadStreams(*donor, donor_path), donor->GroupFileGroups());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", donor_name.c_str(), error.what());
        return 2;
    }

    // An archive that the tools didn't lay out takes no new item.
    try
    {
        ntdlib::rsar::ReadArchiveModel(built_bytes);
    }
    catch (const std::exception& error)
    {
        std::printf("%s: takes no new item: %s\n", name.c_str(), error.what());
        return 0;
    }

    WiiDonorTotals totals;

    // An attempt gives the reason why the check leaves the item out, or nothing.
    const auto attempt = [&name, &totals](const std::string& item, const auto& check) -> std::string
    {
        try
        {
            if (const std::string problem = check(); !problem.empty())
            {
                std::printf("%s: %s %s\n", name.c_str(), item.c_str(), problem.c_str());
                totals.failed++;
            }
        }
        catch (const Refusal& error)
        {
            totals.refused[error.what()]++;
        }
        catch (const LeftOut& error)
        {
            const std::string why = error.what();
            totals.unlabelled += why == "starts where its file has no label" ? 1 : 0;
            totals.without_bank += why == "without a bank in its first slot" ? 1 : 0;
            totals.lacked += why.starts_with("lacks") ? 1 : 0;
            totals.several_slots += why == kSeveralSlots ? 1 : 0;
            totals.trimmed += why == kTrimmed ? 1 : 0;
            return why;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s fails: %s\n", name.c_str(), item.c_str(), error.what());
            totals.failed++;
        }

        return {};
    };

    // Up to 8 of the donor's sounds, wave sound files or wave archives, spread over the donor's tables.
    const auto spread = [](std::size_t count)
    {
        std::vector<uint32_t> chosen;
        for (std::size_t k = 0; k < std::min<std::size_t>(count, 8); k++)
        {
            chosen.push_back(static_cast<uint32_t>(k * count / std::min<std::size_t>(count, 8)));
        }

        return chosen;
    };

    const std::vector<uint32_t> sequences = WiiModels(*built, ntdlib::rsar::SoundType::kSequence, 1);
    const std::vector<uint32_t> wave_sounds = WiiModels(*built, ntdlib::rsar::SoundType::kWaveSound, 1);
    if (mode == "--new")
    {
        std::vector<uint32_t> donor_sounds;
        for (uint32_t s = 0; s < donor->Sounds().size(); s++)
        {
            donor_sounds.push_back(donor->Sounds()[s].type == sar::SoundType::kSequence ? s : sar::kNoItem);
        }
        std::erase(donor_sounds, sar::kNoItem);

        // A sound whose conversion File > Import can't match gives its place to the next of the donor's sounds, up to
        // the next sound that the spread gives.
        const std::vector<uint32_t> chosen = spread(donor_sounds.size());
        for (std::size_t c = 0; c < chosen.size() && !sequences.empty(); c++)
        {
            const std::size_t end = c + 1 < chosen.size() ? chosen[c + 1] : donor_sounds.size();
            for (std::size_t k = chosen[c]; k < end; k++)
            {
                const uint32_t sound = donor_sounds[k];
                const auto check_sound = [&]
                {
                    return DonorSoundProblem(file, *donor, donor_file, sound, sequences[0], build, 10.0, totals);
                };
                const std::string left_out = attempt(donor->Sounds()[sound].name, check_sound);
                if (left_out != kSeveralSlots && left_out != kTrimmed)
                {
                    break;
                }
            }
        }
    }
    else if (mode == "--wave-sounds")
    {
        const std::vector<song::WaveSoundSet> sets = song::WaveSoundSets(*donor);
        const std::vector<uint32_t> models = wave_sounds.empty() ? sequences : wave_sounds;
        for (const uint32_t k : spread(sets.size()))
        {
            if (!models.empty())
            {
                attempt(sets[k].name, [&] { return DonorWaveSoundsProblem(file, *donor, sets[k], models[0], totals); });
            }
        }
    }
    else if (mode == "--new-waves")
    {
        const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*built);
        for (const uint32_t k : spread(donor->WaveArchives().size()))
        {
            if (!wave_data.empty())
            {
                const auto check_wave = [&]
                {
                    return DonorWaveProblem(file, built, built_bytes, *donor, k, wave_data[0].file_id, totals);
                };
                attempt(donor->WaveArchives()[k].name, check_wave);
            }
        }
    }
    else
    {
        // Each of up to 8 banks and 8 wave sound files of the Wii takes the donor's first file of its kind that the
        // donor holds.
        const auto first = [&donor](edit::FileKind kind) -> std::optional<std::vector<uint8_t>>
        {
            const std::size_t count = kind == edit::FileKind::kBank ? donor->Banks().size() : donor->Sounds().size();
            for (uint32_t i = 0; i < count; i++)
            {
                if (kind == edit::FileKind::kWaveSoundFile && donor->Sounds()[i].type != sar::SoundType::kWaveSound)
                {
                    continue;
                }

                const std::span<const uint8_t> data = donor->FileData(
                    kind == edit::FileKind::kBank ? donor->Banks()[i].file_id : donor->Sounds()[i].file_id);
                if (!data.empty())
                {
                    return std::vector<uint8_t>(data.begin(), data.end());
                }
            }

            return std::nullopt;
        };

        const sar::Flavor flavor = donor->ReadOptions().flavor;
        for (const edit::FileKind kind : {edit::FileKind::kBank, edit::FileKind::kWaveSoundFile})
        {
            const std::optional<std::vector<uint8_t>> bytes = first(kind);
            const ntdlib::rsar::SoundType type = kind == edit::FileKind::kBank ? ntdlib::rsar::SoundType::kSequence
                                                                               : ntdlib::rsar::SoundType::kWaveSound;
            std::set<uint32_t> done;
            for (const uint32_t sound : WiiModels(*built, type, 64))
            {
                const ntdlib::rsar::SoundInfo& info = built->Sounds()[sound];
                const uint32_t item = kind == edit::FileKind::kBank ? info.bank : sound;
                if (!bytes || done.size() == 8 || (kind == edit::FileKind::kBank && item >= built->Banks().size()))
                {
                    continue;
                }
                const uint32_t id = kind == edit::FileKind::kBank ? built->Banks()[item].file_id : info.file_id;
                if (!done.insert(id).second || built->FileData(id).empty())
                {
                    continue;
                }
                const auto check_file = [&]
                {
                    return DonorFileProblem(file, built, built_bytes, kind, item, id, *bytes, flavor, totals);
                };
                attempt((kind == edit::FileKind::kBank ? "bank " : "wave sound file ") + std::to_string(item),
                        check_file);
            }
        }
    }

    std::printf("%s: %zu imports from %s (%s). %zu fail.", name.c_str(), totals.imports, donor_name.c_str(),
                mode.c_str(), totals.failed);
    if (mode == "--new")
    {
        std::printf(
            " %zu sequence sounds came in with their banks. %zu play the samples of Another Console's "
            "conversion, and the renders of %zu hold sound. %zu conversions took the balance mode and the "
            "square-root curve. %zu wave archives came in as banks with %zu waves. Of the donor's sounds, %zu "
            "start where their files have no label, %zu have no bank in their first slot and %zu lack their files. "
            "%zu conversions take the banks of several slots, and %zu keep only the instruments that their sounds "
            "select: File > Import can't match them, and the next sounds took their places.",
            totals.sounds, totals.same_samples, totals.sounding, totals.defaults, totals.wave_archives, totals.waves,
            totals.unlabelled, totals.without_bank, totals.lacked, totals.several_slots, totals.trimmed);
    }
    else if (mode == "--wave-sounds")
    {
        std::printf(
            " %zu new wave sounds play the donor's waves. %zu wave archives came in as banks with %zu waves. "
            "%zu of the donor's wave sound files lack their files or their wave archives.",
            totals.wave_sounds, totals.wave_archives, totals.waves, totals.lacked);
    }
    else if (mode == "--new-waves")
    {
        std::printf(
            " %zu new waves keep the donor's samples, after the waves and in a wave's place. %zu of the "
            "donor's waves lack their files.",
            totals.waves, totals.lacked);
    }
    else
    {
        std::printf(" %zu files took a donor's file in their place, %zu of them banks, in %zu copies.", totals.files,
                    totals.banks, totals.copies);
    }
    std::printf("\n");
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }

    return totals.failed == 0 ? 0 : 1;
}

// Prints the summary of the sequences of the Wii's archive `name` with `sequences`, and of its files with `files`.
void PrintWiiSequencesAndFiles(const std::string& name, const WiiTotals& totals, bool sequences, bool files)
{
    if (sequences)
    {
        std::printf(
            "%s: %zu sequence sounds took their files again. %zu took another sound's file. %zu fail. %zu sounds' "
            "files stay in the archive's bytes. %zu sounds start past the first command, where their files have no "
            "label, and Replace… offers their starts. %zu sounds open tracks that they don't allocate. With another "
            "sound's file, ntdlib's performer found the loop of %zu sounds a whole number of passes away. The values "
            "of %zu sounds, such as a volume of 0, decide another length for the other sound's notes.\n",
            name.c_str(), totals.sounds, totals.crossed, totals.failed, totals.in_bytes, totals.unlabelled,
            totals.untracked, totals.loops_moved, totals.other_lengths);
    }
    if (files)
    {
        std::printf(
            "%s: %zu files of banks, wave archives and wave sounds took their bytes again, and %zu took another "
            "file's, in %zu copies. %zu fail. %zu times a next file kept its waves otherwise, and %zu files had no "
            "next file of their kind.\n",
            name.c_str(), totals.files, totals.files_crossed, totals.copies, totals.files_failed, totals.other_form,
            totals.without_next);
    }
}

// The Wii's streams. With --streams, each stream beside an archive of the Wii goes in again with the first stream sound
// that plays it (WiiStreamProblem). With a donor after the archive, each stream beside the donor goes in as a new
// stream sound instead (WiiDonorStreamProblem).

// What the Wii's streams counted.
struct WiiStreamTotals
{
    std::size_t streams = 0; // streams beside the archive that a sound plays, or the donor's streams
    std::size_t failed = 0;
    std::size_t channels = 0;
    std::size_t unplayed = 0;          // streams beside the archive that no sound plays
    std::size_t conversions = 0;       // streams that came back from a file of another console
    std::size_t exact_conversions = 0; // conversions that came back with the stream's bytes
    std::size_t same_wav = 0;          // streams that came back from their WAV files in 16-bit PCM with their bytes
    std::size_t moved = 0;             // streams whose loop's start moved forward to a block of 16-bit PCM
    std::size_t exact = 0;             // streams that came back exactly as DSP-ADPCM
    std::vector<double> ratios;        // the signal-to-noise ratios of the others, in decibels
    std::size_t entries = 0;           // the game's stream sounds that play a stream beside the archive
    std::size_t whole_entries = 0;     // those whose entries name every channel and track of the stream
    std::size_t new_sounds = 0;        // new stream sounds
    std::size_t same_as_model = 0;     // new sounds that played as their models play the stream
    std::size_t sounding = 0;          // new sounds whose renders hold sound
    std::size_t paired = 0;            // donors' streams that took a track for each pair of their channels
    std::size_t pairs = 0;             // second new sounds after a first
    double slowest = 0.0;              // the longest time that a new sound's edit took, in seconds
    std::map<std::string, std::size_t> refused; // the edits that Hokuto refused, by their messages
};

// Whether `info`, the entry of a stream sound of a Wii archive of `version`, names every channel and track of `stream`:
// the channels by their count from version 1.4 on, and by a bit for each before.
bool NamesWholeStream(const ntdlib::rsar::SoundInfo& info, uint16_t version, const ntdlib::rsar::StreamFile& stream)
{
    const auto channels = static_cast<uint32_t>(stream.channels.size());
    const uint32_t named = version >= 0x0104 ? channels : (1U << channels) - 1;
    return info.allocate_channels == named && info.allocate_track_flags == (1U << stream.tracks.size()) - 1;
}

// The first 2 s of what `preview` plays, as floats.
std::vector<float> WiiPreviewSamples(song::SoundPreview& preview)
{
    constexpr std::size_t kBlock = 256;
    const std::size_t frames = 2 * static_cast<std::size_t>(preview.SampleRate());
    std::vector<float> samples(frames * 2);
    std::size_t done = 0;
    while (done < frames)
    {
        const std::size_t rendered = preview.Render(samples.data() + done * 2, std::min(kBlock, frames - done));
        if (rendered == 0)
        {
            break;
        }
        done += rendered;
    }
    samples.resize(done * 2);

    return samples;
}

// Whether each later pass of the loop of `stream`, a stream of the Wii, decodes to the samples of its first pass in
// `decoded`. A channel in DSP-ADPCM decodes from the loop's predictor, scale and history, with the Wii's sums.
bool WiiLaterPassesMatch(const ntdlib::rsar::StreamFile& stream, const sar::Pcm& decoded)
{
    if (!stream.loop || stream.encoding != ntdlib::rsar::WaveEncoding::kDspAdpcm)
    {
        return true;
    }

    for (std::size_t c = 0; c < stream.channels.size(); c++)
    {
        const ntdlib::rsar::StreamFile::Channel& held = stream.channels[c];
        ntdlib::wave::EncodedWave wave;
        wave.encoding = ntdlib::wave::Encoding::kDspAdpcm;
        wave.sum = ntdlib::wave::DspAdpcmSum::kWide;
        wave.loop_end = stream.sample_count - stream.loop_start;
        ntdlib::wave::EncodedChannel& encoded = wave.channels.emplace_back();
        encoded.data = std::span(held.samples).subspan(stream.loop_start / 14 * 8);
        encoded.coefs = &held.adpcm->coefs;
        encoded.yn1 = held.adpcm->loop_yn1;
        encoded.yn2 = held.adpcm->loop_yn2;
        const std::vector<int16_t> pass = ntdlib::wave::DecodeWave(wave).channels.at(0);
        const std::vector<int16_t>& first = decoded.channels.at(c);
        if (first.size() < stream.sample_count || pass.size() < stream.sample_count - stream.loop_start ||
            !std::equal(first.begin() + stream.loop_start, first.begin() + stream.sample_count, pass.begin()))
        {
            return false;
        }
    }

    return true;
}

// What is wrong with `added`, the Wii's `file` after its archive gains stream sound `name`, a copy of sound `model`
// that plays `made` from a new file beside the archive, or nothing. `built` is the archive that `file` builds, and
// `before` its model. With `same_stream`, `made` holds the stream that the model plays.
std::string WiiNewStreamProblem(const format::File& file, const WiiArchive& built,
                                const ntdlib::rsar::ArchiveModel& before, const format::File& added, uint32_t model,
                                const std::string& name, const edit::NewStream& made, const ntdlib::SndBuild& build,
                                bool same_stream, WiiStreamTotals& totals)
{
    hokuto::tools::CheckSaved(added, true);
    totals.new_sounds++;
    const std::vector<uint8_t> after_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(after_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel now = ntdlib::rsar::ReadArchiveModel(after_bytes);
    const auto sound = static_cast<uint32_t>(before.sounds.size());
    const auto id = static_cast<uint32_t>(before.files.size());
    if (now.sounds.size() != sound + 1 || now.files.size() != id + 1 || after->Sounds().size() != sound + 1)
    {
        return "doesn't give the archive one sound and one file more";
    }

    // The new file lies beside the archive at the new path, with the stream's size. The file keeps the new stream as
    // Hokuto's, and the other streams as they were.
    const ntdlib::rsar::FileModel beside{.path = made.path, .path_size = static_cast<uint32_t>(made.bytes.size())};
    if (now.files[id] != beside || song::StreamPath(*after, id) != std::filesystem::path(made.path))
    {
        return "gives the new file another path or size";
    }
    const auto found = added.native->streams.find(id);
    if (found == added.native->streams.end() || *found->second.bytes != made.bytes || found->second.from_game ||
        added.native->streams.size() != file.native->streams.size() + 1)
    {
        return "doesn't keep the new stream as Hokuto's";
    }
    for (const auto& [kept_id, stream] : file.native->streams)
    {
        const auto kept = added.native->streams.find(kept_id);
        if (kept == added.native->streams.end() || kept->second != stream)
        {
            return "changes stream file " + std::to_string(kept_id);
        }
    }

    // The new sound takes the model's settings, and its entry names every channel and track of the stream.
    const ntdlib::rsar::SoundModel& copied = before.sounds[model];
    const ntdlib::rsar::StreamFile stream = ntdlib::rsar::StreamFile::Read(made.bytes);
    const auto channels = static_cast<uint32_t>(stream.channels.size());
    ntdlib::rsar::SoundModel expected;
    expected.name = name;
    expected.type = ntdlib::rsar::SoundType::kStream;
    expected.file_id = id;
    expected.player_id = copied.player_id;
    expected.volume = copied.volume;
    expected.player_priority = copied.player_priority;
    expected.remote_filter = copied.remote_filter;
    expected.user_params = copied.user_params;
    expected.pan_mode = copied.pan_mode;
    expected.pan_curve = copied.pan_curve;
    expected.actor_player_id = copied.actor_player_id;
    expected.settings_3d = copied.settings_3d;
    expected.allocate_channel_count = static_cast<uint16_t>(before.version >= 0x0104 ? channels : (1U << channels) - 1);
    expected.stream_track_flags = static_cast<uint16_t>((1U << stream.tracks.size()) - 1);
    if (now.sounds[sound] != expected)
    {
        return "gives the new sound other values than the model's and the stream's";
    }

    // The reader gives the entry that the Import window plays.
    const ntdlib::rsar::SoundInfo& read = after->Sounds()[sound];
    const ntdlib::rsar::SoundInfo& offered = *made.wii_sound;
    if (read.name != offered.name || read.file_id != offered.file_id || read.type != offered.type ||
        read.player_id != offered.player_id || read.volume != offered.volume ||
        read.player_priority != offered.player_priority || read.remote_filter != offered.remote_filter ||
        !std::ranges::equal(read.user_params, offered.user_params) || read.pan_mode != offered.pan_mode ||
        read.pan_curve != offered.pan_curve || read.allocate_channels != offered.allocate_channels ||
        read.allocate_track_flags != offered.allocate_track_flags)
    {
        return "reads otherwise than the entry that the Import window plays";
    }

    // The other items, files and groups stay, and so do the file's sequences. The new file joins no group.
    if (std::string problem = WiiOthersProblem(before, now, {}); !problem.empty())
    {
        return problem;
    }
    if (added.sequences != file.sequences)
    {
        return "changes the file's sequences";
    }

    // The Import window's Play gives the samples of the browser's Play after the import. Where the model's entry names
    // every channel and track of the stream that it plays, the model plays them too.
    song::SoundPreview offered_play(built, offered, made.bytes, build);
    song::SoundPreview after_play(after, sound, made.bytes, build);
    const std::vector<float> samples = WiiPreviewSamples(after_play);
    if (WiiPreviewSamples(offered_play) != samples)
    {
        return "plays otherwise than the Import window plays it";
    }
    totals.sounding += std::ranges::any_of(samples, [](float sample) { return sample != 0.0f; }) ? 1 : 0;
    const ntdlib::rsar::SoundInfo& model_info = built->Sounds()[model];
    if (same_stream && model_info.type == ntdlib::rsar::SoundType::kStream &&
        NamesWholeStream(model_info, built->Version(), stream))
    {
        song::SoundPreview model_play(built, model, made.bytes, build);
        if (WiiPreviewSamples(model_play) != samples)
        {
            return "plays otherwise than its model plays the stream";
        }
        totals.same_as_model++;
    }

    return {};
}

// What is wrong with stream file `id` beside the Wii's archive of `file`, or nothing. Stream sound `model` plays the
// stream. `built` is the archive that `file` builds from `built_bytes`, and `before` its model. An archive that the
// tools didn't lay out lacks a model. With `second`, the file in the model form takes the stream from its WAV file too,
// and a second new sound goes in after the first.
std::string WiiStreamProblem(const format::File& file, const WiiArchive& built, std::span<const uint8_t> built_bytes,
                             const std::optional<ntdlib::rsar::ArchiveModel>& before, const ntdlib::SndBuild& build,
                             uint32_t id, uint32_t model, bool second, WiiStreamTotals& totals)
{
    const std::vector<uint8_t>& bytes = *file.native->streams.at(id).bytes;
    const ntdlib::rsar::StreamFile present = ntdlib::rsar::StreamFile::Read(bytes);
    const edit::StreamChanges changes = Refusable([&] { return edit::ChangesOfWiiStream(file, *built, model); });
    totals.channels += present.channels.size();

    // The stream itself.
    const edit::IncomingStream same = edit::ReadIncomingWiiStreamFile(bytes, changes.wii_tracks);
    if (same.converted || same.bytes != bytes ||
        Refusable([&] { return edit::ReplaceStream(file, id, same.bytes); }) != file)
    {
        return "doesn't give the file back with its stream";
    }

    // Its files that the browser's Export… writes for the 3DS, the Wii U and the Switch. Each comes back with the
    // stream's samples and tracks.
    const sar::Pcm decoded = song::DecodeStreamFile(bytes);
    for (const sar::Flavor flavor : {sar::Flavor::kCtr, sar::Flavor::kCafe, sar::Flavor::kNx})
    {
        const std::string console = "the " + song::ConsoleName(flavor);
        std::vector<uint8_t> other;
        try
        {
            other = song::ConvertStreamFile(bytes, flavor, song::ExportVersions(song::ItemKind::kStream, flavor).file);
        }
        catch (const std::runtime_error& error)
        {
            totals.refused["a file for " + console + ": " + error.what()]++;
            continue;
        }
        const edit::IncomingStream back = edit::ReadIncomingWiiStreamFile(other, changes.wii_tracks);
        if (!back.converted || back.converted->flavor != flavor ||
            !PlaysSame(decoded, song::DecodeStreamFile(back.bytes)))
        {
            return "comes back from " + console + " with other samples";
        }
        if (ntdlib::rsar::StreamFile::Read(back.bytes).tracks != present.tracks)
        {
            return "comes back from " + console + " with other tracks";
        }
        totals.conversions++;
        totals.exact_conversions += back.bytes == bytes ? 1 : 0;
    }

    // The WAV file in 16-bit PCM: the same samples and tracks. An export writes the new stream's size into the file
    // table, and nothing else changes. The stream itself then gives the archive back.
    const std::vector<uint8_t> wav = song::StreamAsWav(bytes);
    const edit::IncomingStream pcm = edit::ReadIncomingWiiStreamWav(wav, sar::WaveEncoding::kPcm16, changes.wii_tracks);
    if (!PlaysSame(decoded, song::DecodeStreamFile(pcm.bytes)))
    {
        return "plays other samples from its WAV file in 16-bit PCM";
    }
    if (ntdlib::rsar::StreamFile::Read(pcm.bytes).tracks != present.tracks)
    {
        return "doesn't keep the present stream's tracks from its WAV file";
    }
    totals.moved += pcm.loop_moved > 0 ? 1 : 0;
    const format::File replaced = Refusable([&] { return edit::ReplaceStream(file, id, pcm.bytes); });
    hokuto::tools::CheckSaved(replaced, true);
    if (pcm.bytes == bytes)
    {
        totals.same_wav++;
        if (replaced != file)
        {
            return "doesn't give the file back with its stream from its WAV file";
        }
    }
    else
    {
        if (replaced.native->streams.at(id).from_game || *replaced.native->streams.at(id).bytes != pcm.bytes)
        {
            return "doesn't keep the stream from its WAV file as Hokuto's";
        }
        const std::vector<uint8_t> exported = format::ExportArchive(replaced);
        std::vector<std::size_t> differing;
        for (std::size_t i = 0; i < std::min(exported.size(), built_bytes.size()); i++)
        {
            if (exported[i] != built_bytes[i])
            {
                differing.push_back(i);
            }
        }
        if (exported.size() != built_bytes.size() || (!differing.empty() && differing.back() - differing.front() >= 4))
        {
            return "changes more of the archive than the stream's size in the file table";
        }
        if (before)
        {
            ntdlib::rsar::ArchiveModel expected = *before;
            expected.files.at(id).path_size = static_cast<uint32_t>(pcm.bytes.size());
            if (ntdlib::rsar::ReadArchiveModel(exported) != expected)
            {
                return "doesn't give the file table the new stream's size";
            }
        }
        if (!std::ranges::equal(
                format::ExportArchive(Refusable([&] { return edit::ReplaceStream(replaced, id, bytes); })),
                built_bytes))
        {
            return "doesn't give the archive back with the stream again";
        }
        if (second && before)
        {
            const format::File in_model = format::InModelForm(replaced);
            if (format::ExportArchive(in_model) != exported || format::Read(format::Write(in_model)) != in_model ||
                format::ReadText(format::WriteText(in_model)) != in_model)
            {
                return "exports otherwise, or doesn't save and open again, in the model form";
            }
        }
    }

    // The WAV file in DSP-ADPCM. A loop starts at a block, and its later passes decode to its first.
    const edit::IncomingStream adpcm =
        edit::ReadIncomingWiiStreamWav(wav, sar::WaveEncoding::kDspAdpcm, changes.wii_tracks);
    const ntdlib::rsar::StreamFile adpcm_stream = ntdlib::rsar::StreamFile::Read(adpcm.bytes);
    if (adpcm_stream.tracks != present.tracks)
    {
        return "doesn't keep the present stream's tracks from its WAV file in DSP-ADPCM";
    }
    if (adpcm_stream.loop != present.loop ||
        (present.loop &&
         (adpcm_stream.loop_start < present.loop_start || adpcm_stream.loop_start % adpcm_stream.block_samples != 0)))
    {
        return "comes back from its WAV file in DSP-ADPCM with its loop elsewhere";
    }
    if (!WiiLaterPassesMatch(adpcm_stream, song::DecodeStreamFile(adpcm.bytes)))
    {
        return "decodes a later pass of its loop in DSP-ADPCM otherwise than its first";
    }
    if (adpcm.signal_to_noise)
    {
        totals.ratios.push_back(*adpcm.signal_to_noise);
    }
    else
    {
        totals.exact++;
    }

    // A new sound that copies the sound, with the stream from its file. An archive that the tools didn't lay out takes
    // none, and the tool counts the refusal.
    const std::string name = "HOKUTO_CHECK_STRM_" + std::to_string(id);
    const edit::NewStream made = Refusable([&] { return edit::MakeNewWiiStream(*built, model, name, same); });
    if (made.bytes != bytes || !made.file_tracks || !made.tracks_in_file || !made.wii_sound)
    {
        return "doesn't go in as it is as a new sound's stream";
    }
    if (!before)
    {
        Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
        return "takes a new sound in an archive that the tools didn't lay out";
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
    totals.slowest =
        std::max(totals.slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = WiiNewStreamProblem(file, built, *before, added, model, name, made, build, true, totals);
        !problem.empty())
    {
        return problem;
    }
    if (!second)
    {
        return {};
    }

    // A second new sound from the WAV file, with a sound of another kind as its model where the archive holds one, in
    // both forms. A model at volume 0 would leave the new sound silent. The file saves and opens again.
    const std::vector<uint8_t> added_bytes = format::ExportArchive(added);
    const WiiArchive after = song::LoadWiiArchive(added_bytes, "after.brsar", ntdlib::rvl::snd::kRsarOptions);
    const ntdlib::rsar::ArchiveModel after_model = ntdlib::rsar::ReadArchiveModel(added_bytes);
    const auto audible_other = [](const ntdlib::rsar::SoundInfo& sound)
    {
        return sound.type != ntdlib::rsar::SoundType::kStream && sound.volume > 0;
    };
    const auto other = std::ranges::find_if(after->Sounds(), audible_other);
    const auto second_model =
        other != after->Sounds().end() ? static_cast<uint32_t>(other - after->Sounds().begin()) : model;
    const std::string second_name = name + "_2";
    const edit::NewStream second_made =
        Refusable([&] { return edit::MakeNewWiiStream(*after, second_model, second_name, pcm); });
    const format::File twice =
        Refusable([&] { return edit::AddWiiStreamSound(added, second_model, second_name, second_made); });
    if (std::string problem = WiiNewStreamProblem(added, after, after_model, twice, second_model, second_name,
                                                  second_made, build, false, totals);
        !problem.empty())
    {
        return "after a first new sound, " + problem;
    }
    const format::File in_model = format::InModelForm(added);
    const format::File twice_in_model =
        Refusable([&] { return edit::AddWiiStreamSound(in_model, second_model, second_name, second_made); });
    if (twice_in_model.form != in_model.form || format::ExportArchive(twice_in_model) != format::ExportArchive(twice))
    {
        return "adds a second new sound otherwise in the model form";
    }
    for (const format::File& form : {twice, twice_in_model})
    {
        if (format::Read(format::Write(form)) != form || format::ReadText(format::WriteText(form)) != form)
        {
            return "doesn't save and open again with two new sounds";
        }
    }
    totals.pairs++;

    return {};
}

// What is wrong with `bytes`, a stream file beside a donor, as a new stream sound of the Wii's `file` with sound
// `model` as its model, or nothing. `built` is the archive that `file` builds, and `before` its model. An archive that
// the tools didn't lay out lacks a model. The new sound takes the name of the donor's file `donor_id`. With `save`, the
// file with the new sound saves and opens again.
std::string WiiDonorStreamProblem(const format::File& file, const WiiArchive& built,
                                  const std::optional<ntdlib::rsar::ArchiveModel>& before,
                                  const ntdlib::SndBuild& build, uint32_t model, uint32_t donor_id,
                                  const std::vector<uint8_t>& bytes, bool save, WiiStreamTotals& totals)
{
    const std::string name = "HOKUTO_CHECK_DONOR_" + std::to_string(donor_id);
    const edit::IncomingStream incoming = Refusable([&] { return edit::ReadIncomingWiiStreamFile(bytes); });
    const edit::NewStream made = Refusable([&] { return edit::MakeNewWiiStream(*built, model, name, incoming); });
    const ntdlib::rsar::StreamFile stream = ntdlib::rsar::StreamFile::Read(made.bytes);
    totals.channels += stream.channels.size();
    if (!before)
    {
        Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
        return "takes a new sound in an archive that the tools didn't lay out";
    }

    // The new stream plays the donor's samples. A stream of a later console without tracks takes a track for each pair
    // of its channels.
    if (!PlaysSame(song::DecodeStreamFile(bytes), song::DecodeStreamFile(made.bytes)))
    {
        return "plays other samples than the donor's stream";
    }
    if (!made.file_tracks)
    {
        if (stream.tracks.size() != (stream.channels.size() + 1) / 2)
        {
            return "doesn't take a track for each pair of its channels";
        }
        totals.paired++;
    }
    const auto start = std::chrono::steady_clock::now();
    const format::File added = Refusable([&] { return edit::AddWiiStreamSound(file, model, name, made); });
    totals.slowest =
        std::max(totals.slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (std::string problem = WiiNewStreamProblem(file, built, *before, added, model, name, made, build, false, totals);
        !problem.empty())
    {
        return problem;
    }
    if (save && (format::Read(format::Write(added)) != added || format::ReadText(format::WriteText(added)) != added))
    {
        return "doesn't save and open again with the new sound";
    }

    return {};
}

// The streams beside the donor archive at `path`, of the Wii, the 3DS, the Wii U or the Switch, by their files' IDs.
std::map<uint32_t, std::vector<uint8_t>> DonorStreamsOf(const std::filesystem::path& path)
{
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes))
    {
        throw std::runtime_error(xsflib::Utf8(path.filename()) + " can't be read");
    }
    if (bytes.size() >= 4 && std::string(bytes.begin(), bytes.begin() + 4) == "RSAR")
    {
        const WiiArchive archive =
            song::LoadWiiArchive(bytes, xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
        return song::ReadStreams(*archive, path);
    }

    return song::ReadStreams(*song::OpenArchive(path), path);
}

// The check of the streams beside the Wii's archive at `path`, of `bytes`, with `arguments` as main takes them: each
// stream with the first stream sound that plays it (WiiStreamProblem), or with a donor after the archive, the donor's
// streams as new stream sounds (WiiDonorStreamProblem). Returns main's code.
int CheckWiiStreams(const std::vector<std::string>& arguments, const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes)
{
    // The file with the streams beside the archive, the archive that it builds, and the archive's model.
    const std::string name = xsflib::Utf8(path.filename());
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    WiiArchive built;
    ntdlib::SndBuild build;
    std::map<uint32_t, std::vector<uint8_t>> donor;
    try
    {
        const std::optional<ntdlib::SndBuild> game = hokuto::tools::GameBuildOf(path);
        const WiiArchive opened = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        imported = format::ImportWiiArchive(bytes, game, song::ReadStreams(*opened, path));
        imported = hokuto::tools::CheckedForm(std::move(*imported), bytes);
        build = format::BuildOfWiiArchive(*imported->native);
        built_bytes = format::ExportArchive(*imported);
        built = song::LoadWiiArchive(built_bytes, name, ntdlib::rvl::snd::kRsarOptions);
        if (arguments.size() == 4)
        {
            donor = DonorStreamsOf(xsflib::PathFromUtf8(arguments[3]));
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const format::File& file = *imported;
    std::optional<ntdlib::rsar::ArchiveModel> before;
    try
    {
        before = ntdlib::rsar::ReadArchiveModel(built_bytes);
    }
    catch (const std::exception&)
    {
        before.reset();
    }

    // Each check counts the edits that Hokuto refuses by their messages, and fails on any other error.
    WiiStreamTotals totals;
    const auto attempt = [&name, &totals](const std::string& item, const auto& check)
    {
        try
        {
            if (const std::string problem = check(); !problem.empty())
            {
                std::printf("%s: %s %s\n", name.c_str(), item.c_str(), problem.c_str());
                totals.failed++;
            }
        }
        catch (const Refusal& error)
        {
            totals.refused[error.what()]++;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s fails: %s\n", name.c_str(), item.c_str(), error.what());
            totals.failed++;
        }
        totals.streams++;
    };

    const auto print_refused = [&name, &totals]
    {
        for (const auto& [reason, count] : totals.refused)
        {
            std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
        }
    };

    // With a donor, the donor's streams as new stream sounds. The model alternates between the archive's first stream
    // sound and its first sound of another kind. A model at volume 0 would leave the new sounds silent.
    if (arguments.size() == 4)
    {
        const auto first = [&built](bool stream) -> std::optional<uint32_t>
        {
            const auto audible_of_kind = [stream](const ntdlib::rsar::SoundInfo& sound)
            {
                return (sound.type == ntdlib::rsar::SoundType::kStream) == stream && sound.volume > 0;
            };
            const auto found = std::ranges::find_if(built->Sounds(), audible_of_kind);
            if (found == built->Sounds().end())
            {
                return std::nullopt;
            }

            return static_cast<uint32_t>(found - built->Sounds().begin());
        };
        const std::optional<uint32_t> stream_model = first(true);
        const std::optional<uint32_t> other_model = first(false);
        bool turn = false;
        bool save = true;
        for (const auto& [id, stream] : donor)
        {
            if (!stream_model && !other_model)
            {
                break;
            }

            const uint32_t model =
                std::exchange(turn, !turn) ? other_model.value_or(*stream_model) : stream_model.value_or(*other_model);
            attempt("the donor's stream file " + std::to_string(id),
                    [&, id, model]
                    {
                        return WiiDonorStreamProblem(file, built, before, build, model, id, stream,
                                                     std::exchange(save, false), totals);
                    });
        }

        std::printf(
            "%s: %zu streams of the donor, with %zu channels, went in as new stream sounds. %zu fail. %zu took a "
            "track for each pair of their channels. %zu of the %zu new sounds' renders hold sound. The "
            "slowest edit took %.2f s.\n",
            name.c_str(), totals.streams, totals.channels, totals.failed, totals.paired, totals.sounding,
            totals.new_sounds, totals.slowest);
        print_refused();

        return totals.failed > 0 ? 1 : 0;
    }

    // The entries of the stream sounds whose streams the file holds and ntdlib reads.
    for (const ntdlib::rsar::SoundInfo& sound : built->Sounds())
    {
        const auto stream = file.native->streams.find(sound.file_id);
        if (sound.type != ntdlib::rsar::SoundType::kStream || stream == file.native->streams.end())
        {
            continue;
        }
        try
        {
            const ntdlib::rsar::StreamFile read = ntdlib::rsar::StreamFile::Read(*stream->second.bytes);
            totals.entries++;
            totals.whole_entries += NamesWholeStream(sound, built->Version(), read) ? 1 : 0;
        }
        catch (const std::exception&)
        {
            continue;
        }
    }

    // Each stream with the first stream sound that plays it.
    bool second = true;
    for (const auto& [id, stream] : file.native->streams)
    {
        const auto plays = [id](const ntdlib::rsar::SoundInfo& sound)
        {
            return sound.type == ntdlib::rsar::SoundType::kStream && sound.file_id == id;
        };
        const auto model = std::ranges::find_if(built->Sounds(), plays);
        if (model == built->Sounds().end())
        {
            totals.unplayed++;
            continue;
        }
        const auto sound = static_cast<uint32_t>(model - built->Sounds().begin());
        attempt(model->name,
                [&, id, sound]
                {
                    return WiiStreamProblem(file, built, built_bytes, before, build, id, sound,
                                            std::exchange(second, false), totals);
                });
    }

    const std::vector<double>& ratios = totals.ratios;
    const double least = ratios.empty() ? 0.0 : *std::ranges::min_element(ratios);
    std::printf(
        "%s: %zu streams beside the archive, with %zu channels, took their bytes again, came back from the 3DS, the "
        "Wii U and the Switch and from their WAV files, and went in as new stream sounds. %zu fail. %zu of the %zu "
        "conversions came back with the stream's bytes. %zu streams came back from their WAV files in 16-bit PCM "
        "with their bytes, and %zu loops moved forward to a block there. As DSP-ADPCM, %zu streams came back exactly, "
        "and %zu with a signal-to-noise ratio of at least %.1f dB. %zu of the %zu stream sounds whose streams the "
        "archive holds name every channel and track of the stream. %zu new sounds played as their models play the "
        "stream, and %zu of the %zu new sounds' renders hold sound. %zu second new sounds went in after a first. %zu "
        "streams beside the archive play in no sound. The slowest edit took %.2f s.\n",
        name.c_str(), totals.streams, totals.channels, totals.failed, totals.exact_conversions, totals.conversions,
        totals.same_wav, totals.moved, totals.exact, ratios.size(), least, totals.whole_entries, totals.entries,
        totals.same_as_model, totals.sounding, totals.new_sounds, totals.pairs, totals.unplayed, totals.slowest);
    print_refused();

    return totals.failed > 0 ? 1 : 0;
}

// The check of the Wii's archive at `path`, of `bytes`, with `arguments` as main takes them. Returns main's code.
int CheckWii(const std::vector<std::string>& arguments, const std::filesystem::path& path,
             const std::vector<uint8_t>& bytes)
{
    const bool files_only = arguments[1] == "--files";
    const bool sequences_only = arguments[1] == "--sequences";
    const bool waves_only = arguments[1] == "--waves";
    const bool imports = arguments[1] == "--new" || arguments[1] == "--banks" || arguments[1] == "--wave-sounds" ||
                         arguments[1] == "--new-waves";
    const bool donor = arguments.size() == 4 && (arguments[1] == "--new" || arguments[1] == "--wave-sounds" ||
                                                 arguments[1] == "--new-waves" || arguments[1] == "--files");
    if ((arguments.size() != 2 && !files_only && !sequences_only && !waves_only && !imports) ||
        (arguments.size() == 4 && !donor))
    {
        std::fprintf(stderr,
                     "hokuto_check_replace takes only --sequences, --files, --waves, --new, --banks, "
                     "--wave-sounds, --new-waves and --streams for an archive of the Wii, and a donor only after "
                     "--new, --wave-sounds, --new-waves, --files and --streams\n");
        return 2;
    }

    // The file, the archive that it builds, and where each sound plays in the file.
    const std::string name = xsflib::Utf8(path.filename());
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    WiiArchive built;
    std::vector<std::optional<edit::SoundPlace>> places;
    ntdlib::SndBuild build;
    try
    {
        const std::optional<ntdlib::SndBuild> game = hokuto::tools::GameBuildOf(path);
        imported = format::ImportWiiArchive(bytes, game);
        imported = hokuto::tools::CheckedForm(std::move(*imported), bytes);
        build = format::BuildOfWiiArchive(*imported->native);
        built_bytes = format::ExportArchive(*imported);
        built = song::LoadWiiArchive(built_bytes, name, ntdlib::rvl::snd::kRsarOptions);
        places = edit::SoundPlaces(*built, format::SequencesOfFiles(*imported, built_bytes, *built),
                                   format::StartFieldsOfSounds(*imported, built_bytes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const format::File& file = *imported;
    if (donor)
    {
        return CheckWiiDonor(file, built, built_bytes, build, name, arguments[1], xsflib::PathFromUtf8(arguments[3]));
    }
    if (imports)
    {
        return CheckWiiImports(file, built, built_bytes, build, name, arguments[1]);
    }

    // Each sequence sound with its file (WiiSameProblem), then with the next sequence sound's file (WiiCrossProblem).
    WiiTotals totals;
    std::map<uint32_t, song::Sequence> before;
    for (std::size_t i = 0; i < built->Sounds().size() && !files_only && !waves_only; i++)
    {
        const auto sound = static_cast<uint32_t>(i);
        const ntdlib::rsar::SoundInfo& info = built->Sounds()[i];
        if (info.type != ntdlib::rsar::SoundType::kSequence || built->FileData(info.file_id).empty())
        {
            continue;
        }
        if (!places[i])
        {
            totals.in_bytes++;
            continue;
        }

        std::string problem;
        try
        {
            const Source same = WiiSourceOf(built->FileData(info.file_id), info.start_offset, build);
            totals.unlabelled += info.start_offset != 0 && !StartsAtLabel(*built, sound) ? 1 : 0;
            const bool untracked = same.start && Untracked(same, info);
            totals.untracked += untracked ? 1 : 0;
            problem = WiiSameProblem(file, places, sound, same);

            // The next sequence sound with another file. A sound passes its turn on when it opens tracks that it
            // doesn't allocate.
            for (std::size_t step = 1; step < places.size() && problem.empty() && !untracked; step++)
            {
                const auto other = static_cast<uint32_t>((i + step) % places.size());
                if (!places[other] || places[other]->sequence == places[i]->sequence)
                {
                    continue;
                }

                const ntdlib::rsar::SoundInfo& other_info = built->Sounds()[other];
                const Source source = WiiSourceOf(built->FileData(other_info.file_id), other_info.start_offset, build);
                if (!source.start || Untracked(source, other_info))
                {
                    continue;
                }

                problem =
                    WiiCrossProblem(file, places, *built, built_bytes, build, sound, other, source, before, totals);
                totals.crossed++;
                break;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.failed++;
        }
        totals.sounds++;
    }

    if (!sequences_only && !waves_only)
    {
        CheckWiiFiles(file, *built, built_bytes, name, totals);
    }
    if (!sequences_only && !files_only)
    {
        CheckWiiWaves(file, *built, name, totals);
    }

    PrintWiiSequencesAndFiles(name, totals, !files_only && !waves_only, !sequences_only && !waves_only);
    const std::vector<double>& ratios = totals.ratios;
    const double least = ratios.empty() ? 0.0 : *std::ranges::min_element(ratios);
    if (!sequences_only && !files_only)
    {
        std::printf(
            "%s: %zu waves of %zu wave archives came back from their wave files and their WAV files. %zu wave "
            "archives fail. %zu waves hold more than two channels. As DSP-ADPCM, %zu waves came back exactly, and %zu "
            "with a signal-to-noise ratio of at least %.1f dB. %zu loops moved forward to a frame. %zu waves' loops "
            "hold no samples, and they came back from their WAV files without them.\n",
            name.c_str(), totals.waves, totals.wave_archives, totals.wave_archives_failed, totals.waves_wide,
            totals.exact, ratios.size(), least, totals.loops_forward, totals.empty_loops);
    }
    for (const auto& [reason, count] : totals.refused)
    {
        std::printf("%s: refused %zu times: %s\n", name.c_str(), count, reason.c_str());
    }

    return totals.failed > 0 || totals.files_failed > 0 || totals.wave_archives_failed > 0 ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    const bool files_only = arguments.size() == 3 && arguments[1] == "--files";
    const bool waves_only = arguments.size() == 3 && arguments[1] == "--waves";
    const bool new_only = arguments.size() == 3 && arguments[1] == "--new";
    const bool banks_only = arguments.size() == 3 && arguments[1] == "--banks";
    const bool sequences_only = arguments.size() == 3 && arguments[1] == "--sequences";
    const bool wave_sounds_only = (arguments.size() == 3 || arguments.size() == 4) && arguments[1] == "--wave-sounds";
    const bool new_waves_only = arguments.size() == 3 && arguments[1] == "--new-waves";
    const bool streams_only = (arguments.size() == 3 || arguments.size() == 4) && arguments[1] == "--streams";
    const bool variables_only = arguments.size() == 3 && arguments[1] == "--variables";
    const bool wii_donor = arguments.size() == 4 &&
                           (arguments[1] == "--new" || arguments[1] == "--new-waves" || arguments[1] == "--files");
    if (arguments.size() != 2 && !files_only && !waves_only && !new_only && !banks_only && !sequences_only &&
        !wave_sounds_only && !new_waves_only && !streams_only && !variables_only && !wii_donor)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_replace [--model] [--sequences | --files | --waves | --new | --banks | "
                     "--new-waves | --streams | --variables] <archive>\n"
                     "       hokuto_check_replace [--model] --wave-sounds <archive> [<donor>]\n"
                     "       hokuto_check_replace [--model] --streams <archive> [<donor>]\n"
                     "       hokuto_check_replace [--model] --new | --new-waves | --files <Wii archive> <donor>\n");
        return 2;
    }

    // The file, the archive that it builds, and where each sound plays in the file.
    const std::filesystem::path path =
        xsflib::PathFromUtf8(wave_sounds_only || streams_only || wii_donor ? arguments[2] : arguments.back());
    const std::string name = xsflib::Utf8(path.filename());
    const hokuto::tools::SavesLine saves_line(name);
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    Archive built;
    std::vector<Group> groups;
    std::vector<edit::Group> edit_groups;
    std::vector<std::optional<edit::SoundPlace>> places;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (bytes.size() >= 4 && std::string(bytes.begin(), bytes.begin() + 4) == "RSAR")
        {
            return streams_only ? CheckWiiStreams(arguments, path, bytes) : CheckWii(arguments, path, bytes);
        }
        if (wii_donor)
        {
            std::fprintf(stderr, "%s: a donor after %s serves only an archive of the Wii\n", name.c_str(),
                         arguments[1].c_str());
            return 2;
        }
        const Archive opened = song::OpenArchive(path);
        imported = format::ImportArchive(bytes, opened->ReadOptions().flavor, opened->GroupFiles(),
                                         song::ReadStreams(*opened, path), opened->GroupFileGroups());
        imported = hokuto::tools::CheckedForm(std::move(*imported), bytes);
        built_bytes = format::ExportArchive(*imported);
        built = song::LoadArchive(built_bytes, name, format::ExportGroupFiles(*imported));
        groups = GroupsOf(built_bytes, built->ReadOptions().flavor);
        edit_groups = edit::GroupsOf(*imported, *built);
        places = edit::SoundPlaces(*built, format::SequencesOfFiles(*imported, built_bytes, *built),
                                   format::StartFieldsOfSounds(*imported, built_bytes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    const format::File& file = *imported;

    // With --new, the new sounds alone.
    Totals totals;
    if (new_only)
    {
        CheckNewSounds(file, *built, built_bytes, edit_groups, places, name, totals);
        std::printf(
            "%s: %zu new sounds copied a sequence sound with its file. %zu times the same sound came from a file for "
            "another console. %zu fail. %zu went in after another new sound. %zu models start where no command of "
            "their files starts. %zu models open tracks that they don't allocate, and their new sounds play those "
            "tracks. %zu files for another console decode otherwise, and %zu come back with other biquad filters. %zu "
            "files reach a command that the 3DS lacks. The slowest edit took %.2f s.\n",
            name.c_str(), totals.new_sounds, totals.new_crossed, totals.new_failed, totals.new_pairs,
            totals.new_no_start, totals.new_untracked, totals.new_other_unreached, totals.new_other_biquads,
            totals.new_ctr_refused, totals.new_slowest);
        PrintJoins(name, totals, !edit_groups.empty());

        return totals.new_failed > 0 ? 1 : 0;
    }

    // With --variables, the variables of Replace… alone.
    if (variables_only)
    {
        CheckVariables(file, *built, places, name, totals);
        std::printf(
            "%s: %zu sequence sounds read variables before they set them, and %zu of them took the rounds of moves "
            "and fixed values. %zu edits fail. %zu edits were refused. %zu variables moved, and %zu took fixed values, "
            "%zu of them in another of the sound's variables. The edited sounds played %zu times with starting "
            "values. %zu edits of fixed values left a track running without waits, and played otherwise. %zu sounds "
            "start where no command of their files starts.\n",
            name.c_str(), totals.va_reading, totals.va_checked, totals.va_failed, totals.va_refused, totals.va_moved,
            totals.va_fixed, totals.va_held, totals.va_runs, totals.va_runaway, totals.va_no_start);
        for (const std::string& reason : totals.va_reasons)
        {
            std::printf("%s: refused: %s\n", name.c_str(), reason.c_str());
        }

        return totals.va_failed > 0 ? 1 : 0;
    }

    // With --streams and a donor, the donor's streams as new stream sounds.
    if (streams_only && arguments.size() == 4)
    {
        std::optional<format::File> donor;
        try
        {
            const std::filesystem::path donor_path = xsflib::PathFromUtf8(arguments[3]);
            std::vector<uint8_t> bytes;
            if (!xsflib::ReadFile(donor_path, bytes))
            {
                std::fprintf(stderr, "%s can't be read\n", arguments[3].c_str());
                return 2;
            }
            const Archive opened = song::OpenArchive(donor_path);
            donor = format::ImportArchive(bytes, opened->ReadOptions().flavor, opened->GroupFiles(),
                                          song::ReadStreams(*opened, donor_path), opened->GroupFileGroups());
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", arguments[3].c_str(), error.what());
            return 2;
        }
        CheckDonorStreams(file, *built, *donor, name, totals);
        std::printf(
            "%s: %zu streams of the donor, with %zu channels, went in as new stream sounds. %zu fail. %zu new "
            "sounds hold their models' details. %zu streams took a track for each pair of their channels. %zu "
            "lie in an archive without names, and %zu in a 3DS archive whose layout of stream sounds is "
            "unknown: neither takes a new sound. %zu AAC streams count apart. The slowest edit took %.2f s.\n",
            name.c_str(), totals.st_streams, totals.st_channels, totals.st_failed, totals.st_same_details,
            totals.st_paired, totals.st_nameless, totals.st_unknown, totals.st_aac, totals.st_slowest);

        return totals.st_failed > 0 ? 1 : 0;
    }

    // With --streams, the streams beside the archive alone.
    if (streams_only)
    {
        CheckStreams(file, *built, name, totals);
        const std::vector<double>& ratios = totals.st_ratios;
        const double least = ratios.empty() ? 0.0 : *std::min_element(ratios.begin(), ratios.end());
        std::printf(
            "%s: %zu streams beside the archive, with %zu channels, took their bytes again, came back from the other "
            "console and from their WAV files in 16-bit PCM, and went in as new stream sounds. %zu "
            "fail. %zu went in written again in an older version. %zu sounds play the tracks of their stream files. "
            "%zu loops moved forward to a block of 16-bit PCM. %zu new sounds hold their models' details. %zu second "
            "new sounds went in after a first. %zu lie in an archive without names, which takes no new sound. %zu "
            "streams beside the archive play in no sound. As DSP-ADPCM, %zu streams came back exactly, and %zu with "
            "a signal-to-noise ratio of at least %.1f dB. The slowest edit took %.2f s.\n",
            name.c_str(), totals.st_streams, totals.st_channels, totals.st_failed, totals.st_written, totals.st_tracked,
            totals.st_moved, totals.st_same_details, totals.st_pairs, totals.st_nameless, totals.st_unplayed,
            totals.st_exact, ratios.size(), least, totals.st_slowest);
        if (totals.st_aac_replaced + totals.st_aac_missing > 0)
        {
            const std::vector<double>& aac = totals.st_aac_ratios;
            std::printf(
                "%s: %zu AAC streams took their files, and their WAV files and files for the Wii U encoded "
                "again, after a delay of %u samples. %zu AAC stream that the file lacks took another's WAV "
                "file. The lowest signal-to-noise ratio was %.1f dB.\n",
                name.c_str(), totals.st_aac_replaced, totals.st_aac_delay, totals.st_aac_missing,
                *std::min_element(aac.begin(), aac.end()));
        }

        return totals.st_failed > 0 ? 1 : 0;
    }

    // With --wave-sounds, the new wave sounds alone, from the archive or from a donor.
    if (wave_sounds_only)
    {
        Archive donor = built;
        try
        {
            donor = arguments.size() == 4 ? song::OpenArchive(xsflib::PathFromUtf8(arguments[3])) : built;
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", arguments[3].c_str(), error.what());
            return 2;
        }
        CheckNewWaveSounds(file, *built, edit_groups, *donor, donor == built, name, totals);
        std::printf(
            "%s: %zu wave sound files went in as %zu new wave sounds, %zu of them copies of a sequence sound, with "
            "%zu new wave archives. %zu fail. %zu files went in written again with the archive's versions. %zu new "
            "wave sounds played their donors' samples, and neither side decodes %zu. %zu play wave sounds that no "
            "sound of the donor plays. %zu files' wave archives lack their files. The slowest edit took %.2f s.\n",
            name.c_str(), totals.wsd_files, totals.wsd_sounds, totals.wsd_from_sequences, totals.wsd_wave_archives,
            totals.wsd_failed, totals.wsd_written, totals.wsd_compared, totals.wsd_undecoded, totals.wsd_unplayed,
            totals.wsd_missing, totals.wsd_slowest);
        PrintJoins(name, totals, !edit_groups.empty());

        return totals.wsd_failed > 0 ? 1 : 0;
    }

    // With --new-waves, the new waves alone.
    if (new_waves_only)
    {
        CheckNewWaves(file, *built, groups, name, totals);
        std::printf(
            "%s: %zu waves of %zu wave archives went in again as new waves, from their wave files, from their wave "
            "files for the other consoles and from their WAV files. %zu wave archives fail. %zu lie in neither the "
            "archive nor its group files, or share their file. %zu lie only in the group files beside the archive. "
            "%zu wave archives without waves took the archive's first wave, and %zu in archives without waves took a "
            "WAV file. %zu times a wave came back from another console with other bytes. %zu waves hold another "
            "version than a new wave of their wave archive takes, and %zu of them a later one: they went in written "
            "again. %zu second new waves went in after a first. The slowest edit took %.2f s.\n",
            name.c_str(), totals.nw_waves, totals.nw_wave_archives, totals.nw_failed, totals.wave_archives_out,
            totals.wave_archives_beside, totals.nw_empty, totals.nw_waveless, totals.nw_other_bytes,
            totals.nw_other_versions, totals.nw_written, totals.nw_pairs, totals.nw_slowest);
        PrintDspAdpcm(name, totals, false);

        return totals.nw_failed > 0 ? 1 : 0;
    }

    // With --banks, the new banks and wave archives alone.
    if (banks_only)
    {
        CheckNewBanks(file, built, places, name, totals);
        std::printf(
            "%s: %zu new sounds played copies of their models' banks: %zu new banks with %zu new wave archives. %zu "
            "fail. %zu new sounds stay silent for 10 s, as their models do. %zu models have no bank in their slots. "
            "%zu models' banks or wave archives lack their files. %zu models start where no command of their "
            "files starts, or open tracks that they don't allocate. The slowest edit took %.2f s.\n",
            name.c_str(), totals.bank_checked, totals.new_banks, totals.new_wave_archives, totals.bank_failed,
            totals.bank_silent, totals.bankless, totals.bank_missing, totals.bank_skipped, totals.bank_slowest);

        return totals.bank_failed > 0 ? 1 : 0;
    }

    // Each sequence sound with its file and with the files for the other consoles (SameProblem), then with the next
    // sequence sound's file (CrossProblem).
    std::map<uint32_t, song::Sequence> before;
    std::size_t sequence_joins = 0;
    bool sequence_new_group = false;
    for (std::size_t i = 0; i < built->Sounds().size() && !files_only && !waves_only; i++)
    {
        const auto sound = static_cast<uint32_t>(i);
        const sar::SoundInfo& info = built->Sounds()[i];
        if (info.type != sar::SoundType::kSequence || built->FileData(info.file_id).empty())
        {
            continue;
        }
        if (!places[i])
        {
            totals.in_bytes++;
            continue;
        }

        std::string problem;
        try
        {
            const Source same = SourceOf(built->FileData(info.file_id), info.start_offset);
            totals.unlabelled += info.start_offset != 0 && !StartsAtLabel(*built, sound) ? 1 : 0;
            const bool untracked = same.start && Untracked(same, info);
            totals.untracked += untracked ? 1 : 0;
            problem = SameProblem(file, places, *built, sound, same, totals);

            // The next sequence sound with another file. A sound passes its turn on when it opens tracks that it
            // doesn't allocate.
            for (std::size_t step = 1; step < places.size() && problem.empty() && !untracked; step++)
            {
                const auto other = static_cast<uint32_t>((i + step) % places.size());
                if (!places[other] || places[other]->sequence == places[i]->sequence)
                {
                    continue;
                }

                const sar::SoundInfo& other_info = built->Sounds()[other];
                const Source source = SourceOf(built->FileData(other_info.file_id), other_info.start_offset);
                if (!source.start || Untracked(source, other_info))
                {
                    continue;
                }

                std::optional<format::File> replaced;
                problem = CrossProblem(file, places, *built, groups, sound, other, source, before, replaced,
                                       totals.loops_moved);
                totals.crossed++;

                // The sound joins the groups that the Replace window proposes: those that load the sound and lack a
                // file of its new bank slots. The first sound also joins a new group.
                const bool joining = !edit_groups.empty() && sequence_joins < kJoins;
                if (problem.empty() && (joining || !sequence_new_group))
                {
                    const uint32_t id = sar::ItemId(sar::kSoundItem, sound);
                    const std::vector<uint32_t> files_before = edit::FilesOfItem(*built, id);
                    const Choice chosen = joining
                                              ? ChosenGroups(file, *built, edit_groups, files_before,
                                                             edit::FilesOfItem(*built, id, {.banks = other_info.banks}),
                                                             sequence_joins == 0, totals)
                                              : Choice();
                    if (!chosen.groups.empty())
                    {
                        sequence_joins++;
                        problem = JoinCheck(*replaced, chosen, std::span(&id, 1), files_before, totals);
                    }
                    if (problem.empty() && !sequence_new_group)
                    {
                        sequence_new_group = true;
                        problem = NewGroupCheck(*replaced, *built, std::span(&id, 1), files_before, totals);
                    }
                }
                break;
            }
        }
        catch (const std::exception& error)
        {
            problem = std::string("fails: ") + error.what();
        }

        if (!problem.empty())
        {
            std::printf("%s: %s %s\n", name.c_str(), info.name.c_str(), problem.c_str());
            totals.failed++;
        }
        totals.sounds++;
    }

    const bool files = !waves_only && !sequences_only;
    const bool waves = !files_only && !sequences_only;
    if (files)
    {
        CheckFiles(file, *built, groups, edit_groups, places, name, totals);
    }
    if (waves)
    {
        CheckWaves(file, *built, groups, name, totals);
    }

    // A line for each part that ran.
    if (!files_only && !waves_only)
    {
        std::printf(
            "%s: %zu sequence sounds took their files again, and the files for the other consoles. %zu took another "
            "sound's file. %zu fail. %zu sounds' files stay in the archive's bytes. %zu sounds start past the first "
            "command, where their files have no label, and Replace… offers their starts. %zu times a file came back "
            "otherwise from another console, from bytes that no command reaches, and %zu times with other biquad "
            "filters. %zu files reach a command that the 3DS lacks, and don't go to the 3DS. %zu sounds open tracks "
            "that they don't allocate. With another sound's file, ntdlib's performer found the loop of %zu sounds a "
            "whole number of passes away.\n",
            name.c_str(), totals.sounds, totals.crossed, totals.failed, totals.in_bytes, totals.unlabelled,
            totals.other_unreached, totals.other_biquads, totals.ctr_refused, totals.untracked, totals.loops_moved);
    }
    if (files)
    {
        std::printf(
            "%s: %zu files of banks, wave archives and wave sounds took their bytes again, and %zu took another "
            "file's. %zu fail. %zu files have copies in groups with other bytes. %zu files have copies in groups that "
            "name their waves at other places, and %zu such copies took another file with their places. %zu files "
            "are shared by two file IDs, and %zu lie in neither the archive nor its group files. %zu files lie only in "
            "the group files beside the archive.\n",
            name.c_str(), totals.files, totals.files_crossed, totals.files_failed, totals.files_other_copies,
            totals.files_placed, totals.files_placed_copies, totals.files_shared, totals.files_outside,
            totals.files_beside);
        std::printf(
            "%s: %zu times a file of a bank, a wave archive or a wave sound took the same file from another console. "
            "%zu times a wave archive came back with other bytes, and its waves decode as before. %zu other files of "
            "the kind went in written again with the versions of the present file.\n",
            name.c_str(), totals.files_converted, totals.files_converted_otherwise, totals.files_written);
        std::printf(
            "%s: The sounds of the banks select %zu programs, and %zu program changes take their program or slot from "
            "a value. In %zu banks, the next bank lacks %zu of them. %zu took the present bank's instruments, and %zu "
            "the next bank's first instrument.\n",
            name.c_str(), totals.selected, totals.undecided, totals.fixed_banks, totals.fixed_programs,
            totals.fixes_present, totals.fixes_next);
    }
    if (waves)
    {
        std::printf(
            "%s: %zu waves of %zu wave archives came back from their WAV files. %zu wave archives fail. %zu lie in "
            "neither the archive nor its group files, or share their file. %zu lie only in the group files. %zu waves "
            "hold more than two channels. %zu waves lose a loop that doesn't play. In %zu looped DSP-ADPCM waves, the "
            "loop's later passes differ from the first by up to %d in a sample, up to %.1f ms after the loop's "
            "start.\n",
            name.c_str(), totals.waves, totals.wave_archives, totals.wave_archives_failed, totals.wave_archives_out,
            totals.wave_archives_beside, totals.waves_wide, totals.waves_loop_lost, totals.waves_later,
            totals.later_peak, totals.later_ms);
        std::printf(
            "%s: Each wave also went back in from its wave file, and from its wave files for the other consoles. %zu "
            "waves gave their wave archives other bytes when their files went back in their places. %zu times a wave "
            "came back from another console with other bytes, and it decodes as before.\n",
            name.c_str(), totals.waves_rewritten, totals.waves_other_bytes);
        PrintDspAdpcm(name, totals, true);
    }
    if (!waves_only)
    {
        PrintJoins(name, totals, !edit_groups.empty());
    }

    return totals.failed > 0 || totals.files_failed > 0 || totals.wave_archives_failed > 0 ? 1 : 0;
}
