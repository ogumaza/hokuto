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

#include "check_replace.h"

namespace
{

namespace audio = hokuto::audio;
namespace edit = hokuto::edit;
namespace format = hokuto::format;
namespace sar = ntdlib::sar;
namespace song = hokuto::song;
using hokuto::tools::replace::Archive;
using hokuto::tools::replace::CheckDonorStreams;
using hokuto::tools::replace::CheckFiles;
using hokuto::tools::replace::CheckNewBanks;
using hokuto::tools::replace::CheckNewSounds;
using hokuto::tools::replace::CheckNewWaves;
using hokuto::tools::replace::CheckNewWaveSounds;
using hokuto::tools::replace::CheckStreams;
using hokuto::tools::replace::CheckVariables;
using hokuto::tools::replace::CheckWaves;
using hokuto::tools::replace::CheckWii;
using hokuto::tools::replace::CheckWiiStreams;
using hokuto::tools::replace::CrossProblem;
using hokuto::tools::replace::Group;
using hokuto::tools::replace::GroupsOf;
using hokuto::tools::replace::JoinAndNewGroupCheck;
using hokuto::tools::replace::Joins;
using hokuto::tools::replace::JoinsGroups;
using hokuto::tools::replace::PrintDspAdpcm;
using hokuto::tools::replace::PrintJoins;
using hokuto::tools::replace::SameProblem;
using hokuto::tools::replace::Source;
using hokuto::tools::replace::SourceOf;
using hokuto::tools::replace::StartsAtLabel;
using hokuto::tools::replace::Totals;
using hokuto::tools::replace::Untracked;

// With --new, checks the new sounds alone and prints their summary. Returns the exit code.
int RunNew(const format::File& file, const sar::SoundArchive& built, std::span<const uint8_t> built_bytes,
           std::span<const edit::Group> edit_groups, std::span<const std::optional<edit::SoundPlace>> places,
           const std::string& name)
{
    Totals totals;
    CheckNewSounds(file, built, built_bytes, edit_groups, places, name, totals);
    std::printf(
        "%s: %zu new sounds copied a sequence sound with its file. %zu times the same sound came from a file for "
        "another console. %zu fail. %zu went in after another new sound. %zu models start where no command of "
        "their files starts. %zu models open tracks that they don't allocate, and their new sounds play those "
        "tracks. %zu files for another console decode otherwise, and %zu come back with other biquad filters. %zu "
        "files reach a command that the 3DS lacks. The slowest edit took %.2f s.\n",
        name.c_str(), totals.new_sounds, totals.new_crossed, totals.new_failed, totals.new_pairs, totals.new_no_start,
        totals.new_untracked, totals.new_other_unreached, totals.new_other_biquads, totals.new_ctr_refused,
        totals.new_slowest);
    PrintJoins(name, totals, !edit_groups.empty());

    return totals.new_failed > 0 ? 1 : 0;
}

// With --variables, checks the variables of Replace… alone and prints their summary. Returns the exit code.
int RunVariables(const format::File& file, const sar::SoundArchive& built,
                 std::span<const std::optional<edit::SoundPlace>> places, const std::string& name)
{
    Totals totals;
    CheckVariables(file, built, places, name, totals);
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

// With --streams and a donor, checks the donor's streams as new stream sounds and prints their summary. `donor_name`
// is the donor's argument. Returns the exit code.
int RunDonorStreams(const format::File& file, const sar::SoundArchive& built, const std::string& donor_name,
                    const std::string& name)
{
    Totals totals;
    std::optional<format::File> donor;
    try
    {
        const std::filesystem::path donor_path = xsflib::PathFromUtf8(donor_name);
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(donor_path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", donor_name.c_str());
            return 2;
        }
        const Archive opened = song::OpenArchive(donor_path);
        donor = format::ImportArchive(bytes, opened->ReadOptions().flavor, opened->GroupFiles(),
                                      song::ReadStreams(*opened, donor_path), opened->GroupFileGroups());
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", donor_name.c_str(), error.what());
        return 2;
    }
    CheckDonorStreams(file, built, *donor, name, totals);
    std::printf(
        "%s: %zu streams of the donor, with %zu channels, went in as new stream sounds. %zu fail. %zu new "
        "sounds hold their models' details. %zu streams took a track for each pair of their channels. %zu "
        "lie in an archive without names, and %zu in a 3DS archive whose layout of stream sounds is "
        "unknown: neither takes a new sound. %zu AAC streams count apart. The slowest edit took %.2f s.\n",
        name.c_str(), totals.st_streams, totals.st_channels, totals.st_failed, totals.st_same_details, totals.st_paired,
        totals.st_nameless, totals.st_unknown, totals.st_aac, totals.st_slowest);

    return totals.st_failed > 0 ? 1 : 0;
}

// With --streams, checks the streams beside the archive alone and prints their summary. Returns the exit code.
int RunStreams(const format::File& file, const sar::SoundArchive& built, const std::string& name)
{
    Totals totals;
    CheckStreams(file, built, name, totals);
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

// With --wave-sounds, checks the new wave sounds alone, from the archive or from a donor, and prints their summary.
// `arguments` are the arguments of the tool. Returns the exit code.
int RunWaveSounds(const format::File& file, const Archive& built, std::span<const edit::Group> edit_groups,
                  const std::vector<std::string>& arguments, const std::string& name)
{
    Totals totals;
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

// With --new-waves, checks the new waves alone and prints their summary. Returns the exit code.
int RunNewWaves(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                const std::string& name)
{
    Totals totals;
    CheckNewWaves(file, built, groups, name, totals);
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

// With --banks, checks the new banks and wave archives alone and prints their summary. Returns the exit code.
int RunBanks(const format::File& file, const Archive& built, std::span<const std::optional<edit::SoundPlace>> places,
             const std::string& name)
{
    Totals totals;
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

// What is wrong when sequence sound `sound` of `built`, the archive that `file` builds, receives the file of sound
// `other` (CrossProblem), or nothing. Sound `other` plays `source`. The sound then joins groups (JoinAndNewGroupCheck).
// `before` contains the sequences that the sounds played before. Counts the crossings, the loops that moved and the
// joins.
std::string CrossAndJoin(const format::File& file, const sar::SoundArchive& built, std::span<const Group> groups,
                         std::span<const edit::Group> edit_groups,
                         std::span<const std::optional<edit::SoundPlace>> places, uint32_t sound, uint32_t other,
                         const Source& source, std::map<uint32_t, song::Sequence>& before, Joins& joins, Totals& totals)
{
    const sar::SoundInfo& other_info = built.Sounds()[other];
    std::optional<format::File> replaced;
    std::string problem =
        CrossProblem(file, places, built, groups, sound, other, source, before, replaced, totals.loops_moved);
    totals.crossed++;

    // The sound joins the groups that the Replace window proposes: those that load the sound and lack a
    // file of its new bank slots. The first sound also joins a new group.
    if (problem.empty() && JoinsGroups(joins, edit_groups))
    {
        const uint32_t id = sar::ItemId(sar::kSoundItem, sound);
        const std::vector<uint32_t> files_before = edit::FilesOfItem(built, id);
        const auto files_after = [&]
        {
            return edit::FilesOfItem(built, id, {.banks = other_info.banks});
        };
        const auto added = [&]() -> const format::File&
        {
            return *replaced;
        };
        problem = JoinAndNewGroupCheck(file, built, edit_groups, std::span(&id, 1), files_before, files_after, added,
                                       joins, totals);
    }

    return problem;
}

// What is wrong when sequence sound `i` of `built`, the archive that `file` builds, receives its file again and the
// files for the other consoles (SameProblem), then the next sequence sound's file (CrossAndJoin), or nothing. `before`
// contains the sequences that the sounds played before.
std::string SequenceSoundProblem(const format::File& file, const sar::SoundArchive& built,
                                 std::span<const Group> groups, std::span<const edit::Group> edit_groups,
                                 std::span<const std::optional<edit::SoundPlace>> places, std::size_t i,
                                 std::map<uint32_t, song::Sequence>& before, Joins& joins, Totals& totals)
{
    const auto sound = static_cast<uint32_t>(i);
    const sar::SoundInfo& info = built.Sounds()[i];
    const Source same = SourceOf(built.FileData(info.file_id), info.start_offset);
    totals.unlabelled += info.start_offset != 0 && !StartsAtLabel(built, sound) ? 1 : 0;
    const bool untracked = same.start && Untracked(same, info);
    totals.untracked += untracked ? 1 : 0;
    std::string problem = SameProblem(file, places, built, sound, same, totals);

    // The next sequence sound with another file. A sound passes its turn on when it opens tracks that it
    // doesn't allocate.
    for (std::size_t step = 1; step < places.size() && problem.empty() && !untracked; step++)
    {
        const auto other = static_cast<uint32_t>((i + step) % places.size());
        if (!places[other] || places[other]->sequence == places[i]->sequence)
        {
            continue;
        }

        const sar::SoundInfo& other_info = built.Sounds()[other];
        const Source source = SourceOf(built.FileData(other_info.file_id), other_info.start_offset);
        if (!source.start || Untracked(source, other_info))
        {
            continue;
        }

        problem = CrossAndJoin(file, built, groups, edit_groups, places, sound, other, source, before, joins, totals);
        break;
    }

    return problem;
}

// Prints a line for each part that ran: the sequence sounds unless `files_only` or `waves_only`, the files unless
// `waves_only` or `sequences_only`, the waves unless `files_only` or `sequences_only`, and the joins, for an archive
// with groups when `groups` says so, unless `waves_only`.
void PrintSummary(const std::string& name, const Totals& totals, bool files_only, bool waves_only, bool sequences_only,
                  bool groups)
{
    const bool files = !waves_only && !sequences_only;
    const bool waves = !files_only && !sequences_only;
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
        PrintJoins(name, totals, groups);
    }
}

// The options of the command line: the part of the check that runs alone, if any, and whether a donor follows a Wii
// archive.
struct Options
{
    bool files_only = false;
    bool waves_only = false;
    bool new_only = false;
    bool banks_only = false;
    bool sequences_only = false;
    bool wave_sounds_only = false;
    bool new_waves_only = false;
    bool streams_only = false;
    bool variables_only = false;
    bool wii_donor = false;
};

// The options of the command line's `arguments`, or nothing after the usage.
std::optional<Options> ParseOptions(const std::vector<std::string>& arguments)
{
    Options o;
    o.files_only = arguments.size() == 3 && arguments[1] == "--files";
    o.waves_only = arguments.size() == 3 && arguments[1] == "--waves";
    o.new_only = arguments.size() == 3 && arguments[1] == "--new";
    o.banks_only = arguments.size() == 3 && arguments[1] == "--banks";
    o.sequences_only = arguments.size() == 3 && arguments[1] == "--sequences";
    o.wave_sounds_only = (arguments.size() == 3 || arguments.size() == 4) && arguments[1] == "--wave-sounds";
    o.new_waves_only = arguments.size() == 3 && arguments[1] == "--new-waves";
    o.streams_only = (arguments.size() == 3 || arguments.size() == 4) && arguments[1] == "--streams";
    o.variables_only = arguments.size() == 3 && arguments[1] == "--variables";
    o.wii_donor = arguments.size() == 4 &&
                  (arguments[1] == "--new" || arguments[1] == "--new-waves" || arguments[1] == "--files");
    if (arguments.size() != 2 && !o.files_only && !o.waves_only && !o.new_only && !o.banks_only && !o.sequences_only &&
        !o.wave_sounds_only && !o.new_waves_only && !o.streams_only && !o.variables_only && !o.wii_donor)
    {
        std::fprintf(stderr,
                     "usage: hokuto_check_replace [--model] [--sequences | --files | --waves | --new | --banks | "
                     "--new-waves | --streams | --variables] <archive>\n"
                     "       hokuto_check_replace [--model] --wave-sounds <archive> [<donor>]\n"
                     "       hokuto_check_replace [--model] --streams <archive> [<donor>]\n"
                     "       hokuto_check_replace [--model] --new | --new-waves | --files <Wii archive> <donor>\n");
        return std::nullopt;
    }

    return o;
}

// The file of an archive of a later console, the archive that it builds, and where each sound plays in the file.
struct ReplaceFile
{
    std::optional<format::File> imported;
    std::vector<uint8_t> built_bytes;
    Archive built;
    std::vector<Group> groups;
    std::vector<edit::Group> edit_groups;
    std::vector<std::optional<edit::SoundPlace>> places;
};

// Imports `bytes`, the archive at `path`, `name`, into a file, and builds the archive again from the file. Throws when
// a step fails.
ReplaceFile OpenReplaceFile(const std::filesystem::path& path, const std::string& name,
                            const std::vector<uint8_t>& bytes)
{
    ReplaceFile opened;
    const Archive archive = song::OpenArchive(path);
    opened.imported = format::ImportArchive(bytes, archive->ReadOptions().flavor, archive->GroupFiles(),
                                            song::ReadStreams(*archive, path), archive->GroupFileGroups());
    opened.imported = hokuto::tools::CheckedForm(std::move(*opened.imported), bytes);
    opened.built_bytes = format::ExportArchive(*opened.imported);
    opened.built = song::LoadArchive(opened.built_bytes, name, format::ExportGroupFiles(*opened.imported));
    opened.groups = GroupsOf(opened.built_bytes, opened.built->ReadOptions().flavor);
    opened.edit_groups = edit::GroupsOf(*opened.imported, *opened.built);
    opened.places =
        edit::SoundPlaces(*opened.built, format::SequencesOfFiles(*opened.imported, opened.built_bytes, *opened.built),
                          format::StartFieldsOfSounds(*opened.imported, opened.built_bytes));

    return opened;
}

// Runs the part of the check that an option of `o` runs alone, on `opened`, the file of the archive `name`, and returns
// the tool's exit status. Returns nothing without such an option. `arguments` are the arguments of the command line.
std::optional<int> RunAlone(const Options& o, const std::vector<std::string>& arguments, const ReplaceFile& opened,
                            const std::string& name)
{
    const auto& [imported, built_bytes, built, groups, edit_groups, places] = opened;
    const format::File& file = *imported;

    // With --new, the new sounds alone.
    if (o.new_only)
    {
        return RunNew(file, *built, built_bytes, edit_groups, places, name);
    }

    // With --variables, the variables of Replace… alone.
    if (o.variables_only)
    {
        return RunVariables(file, *built, places, name);
    }

    // With --streams and a donor, the donor's streams as new stream sounds.
    if (o.streams_only && arguments.size() == 4)
    {
        return RunDonorStreams(file, *built, arguments[3], name);
    }

    // With --streams, the streams beside the archive alone.
    if (o.streams_only)
    {
        return RunStreams(file, *built, name);
    }

    // With --wave-sounds, the new wave sounds alone, from the archive or from a donor.
    if (o.wave_sounds_only)
    {
        return RunWaveSounds(file, built, edit_groups, arguments, name);
    }

    // With --new-waves, the new waves alone.
    if (o.new_waves_only)
    {
        return RunNewWaves(file, *built, groups, name);
    }

    // With --banks, the new banks and wave archives alone.
    if (o.banks_only)
    {
        return RunBanks(file, built, places, name);
    }

    return std::nullopt;
}

// Each sequence sound of `opened`, the file of the archive `name`, with its file and with the files for the other
// consoles (SameProblem), then with the next sequence sound's file (CrossProblem). Counts in `totals`.
void CheckSequenceSounds(const ReplaceFile& opened, const std::string& name, Totals& totals)
{
    const auto& [imported, built_bytes, built, groups, edit_groups, places] = opened;
    const format::File& file = *imported;
    std::map<uint32_t, song::Sequence> before;
    Joins sequence_joins;
    for (std::size_t i = 0; i < built->Sounds().size(); i++)
    {
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
            problem =
                SequenceSoundProblem(file, *built, groups, edit_groups, places, i, before, sequence_joins, totals);
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
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    hokuto::tools::TakeModelOption(arguments);
    const std::optional<Options> options = ParseOptions(arguments);
    if (!options)
    {
        return 2;
    }

    // The file, the archive that it builds, and where each sound plays in the file.
    const std::filesystem::path path = xsflib::PathFromUtf8(
        options->wave_sounds_only || options->streams_only || options->wii_donor ? arguments[2] : arguments.back());
    const std::string name = xsflib::Utf8(path.filename());
    const hokuto::tools::SavesLine saves_line(name);
    ReplaceFile opened;
    try
    {
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(path, bytes))
        {
            std::fprintf(stderr, "%s can't be read\n", name.c_str());
            return 2;
        }
        if (hokuto::tools::IsWiiArchive(bytes))
        {
            return options->streams_only ? CheckWiiStreams(arguments, path, bytes) : CheckWii(arguments, path, bytes);
        }
        if (options->wii_donor)
        {
            std::fprintf(stderr, "%s: a donor after %s serves only an archive of the Wii\n", name.c_str(),
                         arguments[1].c_str());
            return 2;
        }
        opened = OpenReplaceFile(path, name, bytes);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }
    if (const std::optional<int> alone = RunAlone(*options, arguments, opened, name))
    {
        return *alone;
    }

    // Each sequence sound with its file and with the files for the other consoles (SameProblem), then with the next
    // sequence sound's file (CrossProblem).
    Totals totals;
    if (!options->files_only && !options->waves_only)
    {
        CheckSequenceSounds(opened, name, totals);
    }

    const bool files = !options->waves_only && !options->sequences_only;
    const bool waves = !options->files_only && !options->sequences_only;
    if (files)
    {
        CheckFiles(*opened.imported, *opened.built, opened.groups, opened.edit_groups, opened.places, name, totals);
    }
    if (waves)
    {
        CheckWaves(*opened.imported, *opened.built, opened.groups, name, totals);
    }

    // A line for each part that ran.
    PrintSummary(name, totals, options->files_only, options->waves_only, options->sequences_only,
                 !opened.edit_groups.empty());

    return totals.failed > 0 || totals.files_failed > 0 || totals.wave_archives_failed > 0 ? 1 : 0;
}
