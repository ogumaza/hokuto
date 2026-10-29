// SPDX-License-Identifier: MIT

// New items in a file's archive: File > Import > Sequence…, Bank…, Wave Archive…, Wave Sound File… and Stream….
//
// A sequence file of the Switch or the Wii U comes in as a new sequence sound with a new sequence file. The sound
// copies the entry of a model, a sequence sound of the archive: its player, its volume, its pan, its priorities and its
// 3D settings (ntdlib::sar::WithNewSequenceSound). Its bank slots hold the model's banks unless the user chooses others
// for the import. The new sound plays the new file's commands from one of the file's starts, and allocates the tracks
// that they open. A program map changes the programs that the commands select. Replace… uses the same map
// (edit/replace.h). The new file takes the version of the model's file. The new sound goes at the end of the sound
// table, and the new file at the end of the file table. Scenes and tracks' colours name sounds by their numbers. Those
// numbers stay. The game's code calls sounds by their numbers too. Only a change to that code plays the new sound in
// the game.
//
// A bank file or a wave archive file of the archive's console comes in as a new bank or a new wave archive, with a new
// file. A bank names its waves by a wave archive's item ID and a wave's index. The IDs come from the bank's archive. A
// map gives the wave archive of this archive for each of them. The new bank's file names those instead
// (ntdlib::sar::WithWaveArchives). The tools fill a bank entry's list of wave archives in some archives and leave it
// empty in others. The new bank's entry lists its wave archives where another bank's entry in the archive lists wave
// archives. A new wave archive loads whole. Its name is optional: most of the tools' wave archives have none. New items
// go at the end of their tables. A sound plays a new bank once one of its bank slots names it.
//
// A wave sound file of the archive's console comes in as a new file, with a new wave sound for each wave sound that it
// holds. The file names its waves as a bank does, and the same map gives the wave archives of this archive. Each new
// sound copies the entry of a model, a wave sound or a sequence sound of the archive: its player, its volume, its pan,
// its priorities and its 3D settings (ntdlib::sar::WithNewWaveSounds). A sequence sound's details give way to a wave
// sound's, with the model's channel priority. The file sets each wave sound's volume, pan, pitch and envelope.
//
// A stream file of the Switch or the Wii U, or a WAV file, comes in as a new stream sound that plays the stream from a
// new file outside the archive. The game reads a stream from the archive's folder, at the path that the file table
// gives, such as stream/BGM_TITLE.dspadpcm.bfstm. The Hokuto file keeps the new stream, and an export writes it there.
// The new sound copies the entry of a model, a sound of any kind of the archive (ntdlib::sar::WithNewStreamSound). Its
// details give its tracks: the stream's channels that each one plays, with the track's volume, pan and sends.
//
// A new file joins no group by itself. A game loads only what its groups hold, or what its code loads by itself. The
// windows offer the groups, and edit/groups.h puts the new item into those that the user chooses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sar/layout.h"

#include "edit/groups.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "format/file.h"

namespace hokuto::edit
{

// Whether `archive`, the bytes of a sound archive of `flavor`, holds names: a STRG block. The edits refuse a name for a
// new item of an archive without names, and a new sound, bank or group there. An archive whose tables can't be read
// counts as holding names: the edits then give their reasons.
bool HoldsNames(std::span<const uint8_t> archive, ntdlib::sar::Flavor flavor);

// The edits' reason for a name of a new `item`, such as "sound", in an archive without names.
std::string NoNamesReason(const std::string& item);

// The file that `archive` and `group_files` make: the archive of `file` with new items, and the group files beside it.
// The file's other parts stay, the game's sizes and the tags among them. Each group file stays the game's, by its size
// and its SHA-1: an export compares it with the game's. The file's sequences keep their numbers, and the new ones
// follow. The sequences that didn't change stay too: the new file shares them. The cut takes the file's sequences by
// their files' IDs (ntdlib::sar::KnownSequences), and the new files' sequences of `added`: a sequence whose commands
// run on past a return to commands that no label or start leads to comes back too. Throws the exceptions of
// format::ImportArchive, and std::invalid_argument with a message for the user when the new archive lacks one of the
// file's sequences.
format::File Rebuilt(const format::File& file, std::span<const uint8_t> archive,
                     std::vector<std::vector<uint8_t>> group_files, const ntdlib::sar::KnownSequences& added = {});

// The file after its archive gains sequence sound `name`, a copy of sound `model` of the archive's sound table that
// plays `incoming` from item `start`, with `programs` in the place of the programs that its program changes select.
// `banks` gives the new sound's bank slots: bank item IDs, or 0xFFFFFFFF for an empty slot. Without it, the new sound
// takes the model's. The archive's other items stay as they were. So do the file's sequences, its effects, its scenes
// and the tracks' colours. The new sound's sequence comes after the file's other sequences. Without `model`, a file in
// the model form gives the new sound Hokuto's settings: the archive's first player, a volume of 127, the centre and
// priorities of 64. The new sequence file then takes the version that Export… writes for the archive's console
// (song::ExportVersions). Without `banks`, such a sound's one bank slot holds the archive's first bank, or none. Throws
// std::invalid_argument with a message for the user for a file without the archive's tables, a model that isn't a
// sequence sound, no model in a file of the native form, a start past the end of `incoming`, more than four bank slots
// or a bank past the bank table, an archive without names, a name that is empty or that the archive holds already,
// commands that the console's bytecode can't hold, an archive without a player for a sound without a model, and an
// archive that ntdlib can't add the sound to.
format::File AddSequenceSound(const format::File& file, std::optional<uint32_t> model, const std::string& name,
                              const ntdlib::commands::Sequence& incoming, uint32_t start, const ProgramMap& programs,
                              const std::optional<std::vector<uint32_t>>& banks = std::nullopt);

// The size in bytes of the sequence file that AddSequenceSound or AddWiiSequenceSound adds to the archive of `file` for
// `sequence`, the new commands with the programs of the map: the commands as the archive's console writes them, with
// the sequence's labels. A file's version doesn't change its size. On the Wii, the size includes the padding up to a
// multiple of 32 bytes. Throws std::invalid_argument with a message for the user for a file without an archive of a
// game, and for commands that the console's bytecode can't hold.
std::size_t NewSequenceFileSize(const format::File& file, const ntdlib::commands::Sequence& sequence);

// A wave archive that the waves of a bank or a wave sound file name: its item ID in the file's archive, and the indices
// of the waves, in order, each once.
struct NamedWaveArchive
{
    uint32_t id = 0;
    std::vector<uint32_t> waves;
};

// The wave archives that the waves of `file`, a bank file or a wave sound file of `flavor`, name, in the order of their
// first use. The magic gives the kind. Throws std::invalid_argument with a message for the user for a file that ntdlib
// can't read.
std::vector<NamedWaveArchive> WaveArchivesNamed(std::span<const uint8_t> file, ntdlib::sar::Flavor flavor);

// `bytes`, a bank file or a wave sound file of the archive of `file`, after its waves name the wave archives of `map`
// (see AddBank). Replace… of a bank or a wave sound file puts such a file in the item's place. The magic gives the
// kind. Throws std::invalid_argument with a message for the user for a file without the archive's tables, a file that
// ntdlib can't read or change, a wave archive that the map leaves out, and a place past the archive's list.
std::vector<uint8_t> WithWaveArchiveMap(const format::File& file, std::span<const uint8_t> bytes,
                                        const std::map<uint32_t, uint32_t>& map);

// The file after its archive gains wave archive `name` with `bytes`, a wave archive file of the archive's console, as a
// new file. An empty name leaves the wave archive without one. The archive's other items stay as they were. So do the
// file's other parts. Throws std::invalid_argument with a message for the user for a file without the archive's tables,
// a name for an archive without names, a name that the archive holds already, and an archive that ntdlib can't add the
// wave archive to.
format::File AddWaveArchive(const format::File& file, const std::string& name, std::span<const uint8_t> bytes);

// The file after its archive gains groups named `names`, in order, at the end of its group table. Each new group's file
// holds no files and lists no items. It goes after the archive's other files (ntdlib::sar::WithNewGroup). The game's
// code loads a group by its number. Only a change to that code loads a new group. The archive's other items stay as
// they were. So do the file's other parts. Throws std::invalid_argument with a message for the user for a file without
// the archive's tables, an archive without names, a name that is empty or that the archive or another new group holds
// already, and an archive that ntdlib can't add a group to.
format::File AddGroups(const format::File& file, std::span<const std::string> names);

// The file after its archive gains bank `name` with `bytes`, a bank file of the archive's console, as a new file. `map`
// gives a wave archive of this archive, by its place in the archive's list, for each wave archive that the bank's waves
// name, by its item ID (WaveArchivesNamed). The new file names those wave archives instead. The archive's other items
// stay as they were. So do the file's other parts. Throws std::invalid_argument with a message for the user for a file
// without the archive's tables, an archive without names, a name that is empty or that the archive holds already, a
// bank file that ntdlib can't read, a wave archive that the map leaves out, a place past the archive's list, and an
// archive that ntdlib can't add the bank to.
format::File AddBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                     const std::map<uint32_t, uint32_t>& map);

// The names of the new sounds of a wave sound file with `count` wave sounds: `name` for one wave sound, and `name` with
// each wave sound's index after it for more. The indices take zeros in front up to the digits of the count:
// SE_HIT_00 to SE_HIT_11 for 12 wave sounds.
std::vector<std::string> WaveSoundNames(const std::string& name, std::size_t count);

// The wave sounds that `bytes`, a wave sound file of `flavor`, holds. Throws std::invalid_argument with a message for
// the user for a file that ntdlib can't read.
std::size_t WaveSoundCount(std::span<const uint8_t> bytes, ntdlib::sar::Flavor flavor);

// The file after its archive gains `bytes`, a wave sound file of the archive's console, as a new file, and a new sound
// for each of its wave sounds, named by WaveSoundNames(`name`). Each new sound copies sound `model` of the archive's
// sound table, a wave sound or a sequence sound. `map` gives a wave archive of this archive, by its place in the
// archive's list, for each wave archive that the file's waves name, by its item ID (WaveArchivesNamed). The new file
// names those wave archives instead. The archive's other items stay as they were. So do the file's other parts. Throws
// std::invalid_argument with a message for the user for a file without the archive's tables, a model that isn't a wave
// sound or a sequence sound, an archive without names, a name that is empty or that the archive holds already, a wave
// sound file that ntdlib can't read or that holds no wave sound, a wave archive that the map leaves out, a place past
// the archive's list, and an archive that ntdlib can't add the sounds to.
format::File AddWaveSounds(const format::File& file, uint32_t model, const std::string& name,
                           std::span<const uint8_t> bytes, const std::map<uint32_t, uint32_t>& map);

// A new stream sound's stream: the stream file, the path at which the game reads it from the archive's folder, and the
// sound's details as the archive's tables hold them. `file_tracks` says whether the stream file that came in gave the
// tracks. `tracks_in_file` says whether the sound plays the tracks of its stream file: on the 3DS, before version
// 2.3.0.0 of the archive, and on the Wii. The details then hold none. On the Wii, `wii_sound` holds the new sound's
// entry as the sound table will hold it (MakeNewWiiStream).
struct NewStream
{
    std::vector<uint8_t> bytes;
    std::string path;
    ntdlib::sar::StreamSoundInfo details;
    bool file_tracks = false;
    bool tracks_in_file = false;
    std::optional<ntdlib::rsar::SoundInfo> wii_sound;
};

// The stream of `incoming` for a new stream sound named `name` that copies sound `model` of `archive`. The path is
// stream/ and the name, then .dspadpcm.bfstm for a stream in DSP-ADPCM or .bfstm for another encoding. NintendoWare's
// tools name the streams that they encode with .dspadpcm.bfstm. The 3DS's names end in .dspadpcm.bcstm and .bcstm. A
// number follows the name while the archive's file table names the path in any case. The stream file's tracks give the
// sound's tracks where the file holds them. Otherwise each pair of channels makes a stereo track, and a last channel by
// itself a mono track. A track takes the settings of the model's track in its place where the model is a stream sound
// with one there, and otherwise those of a track that sets none: full volume, the centre and the full main send. The
// file's tracks give their volume, pan, span and flags. Every track plays. The sound takes the model's pitch and sends
// where the model is a stream sound. Where the sound plays the tracks of its stream file, a stream file without tracks
// takes the tracks of the pairs of its channels, at full volume and the centre. Throws std::invalid_argument with a
// message for the user for a model past the sound table, a stream that ntdlib can't read or write, a stream without
// channels, and more than 8 tracks. On the 3DS, it throws for more than 4 tracks, a track of more than 2 channels and
// more than 8 channels: the 3DS's player plays no more. It also throws for a 3DS archive of CSAR 2.1.0.0 or 2.3.1.0: no
// test archive shows how the tools lay out the stream sounds of those versions.
NewStream MakeNewStream(const ntdlib::sar::SoundArchive& archive, uint32_t model, const std::string& name,
                        const IncomingStream& incoming);

// The file after its archive gains stream sound `name`, a copy of sound `model` of the archive's sound table that plays
// `stream` from a new file outside the archive, at `stream.path`. The file keeps the stream as Hokuto's: an export
// writes it beside the archive. The archive's other items stay as they were. So do the file's other parts, its other
// streams among them. Throws std::invalid_argument with a message for the user for a file without the archive's tables,
// a model past the sound table, an archive without names, a name that is empty or that the archive holds already, a
// path that the archive's file table names already, details that a stream sound can't hold, and an archive that ntdlib
// can't add the sound to, such as a 3DS archive of a version whose stream sounds ntdlib doesn't lay out.
format::File AddStreamSound(const format::File& file, uint32_t model, const std::string& name, const NewStream& stream);

// The Wii's new items. A sound archive of the Wii (RSAR) keeps every file in its groups (song/items.h). A new item goes
// in through the model of the archive's items (ntdlib::rsar::ReadArchiveModel): its entries at the ends of their
// tables, and its file at the end of the file table. ntdlib lays the archive out again from the model as NintendoWare
// for Revolution's tools do (ntdlib::rsar::BuildArchive), and it gives every archive that the tools laid out back byte
// for byte from its model. The archive's other items therefore keep their values, and its other files their bytes. Only
// their places in the archive move. An archive that the tools laid out otherwise, or one that is cut short, takes no
// new item. The new file goes into each group of `groups`, by its index among the groups with names, after the group's
// files. A group that loads the model takes only the files that the model doesn't require. Another group takes every
// file of the new item that it lacks. Without a group, the new file goes into the archive's last group: the group
// without a name that holds the files that no other group holds. A bank file or a wave sound file comes in with its
// wave data: an RWAR, or the samples that its WAVE block describes. The file's sequences keep their numbers, and a new
// sequence comes after them. The file's other parts stay, the game's sizes and the game's build of NW4R SND among them.
// A file in the model form stays in it.

// The file after its archive of the Wii gains sequence sound `name`, a copy of sound `model` of the archive's sound
// table that plays `incoming` from item `start`, with `programs` in the place of the programs that its program changes
// select. `banks` gives the new sound's one bank slot: a bank item ID (ntdlib::sar::ItemId). Without it, the new sound
// takes the model's bank. The new sequence file takes the version of the model's file. The new sound allocates the
// tracks that its commands open. Without `model`, the new sound takes Hokuto's settings: the archive's first player, a
// volume of 127, priorities of 64 and the 3D settings of an entry that sets none. It then plays the archive's first
// bank, and its sequence file takes the version of the archive's first sequence file, or 1.0. Throws
// std::invalid_argument with a message for the user for a file that isn't of the Wii, a model that isn't a sequence
// sound, a start past the end of `incoming`, a slot without a bank or a bank past the bank table, more than one slot,
// an archive without a bank or a player for the new sound, a name that is empty or that the archive holds already,
// commands that the build's bytecode can't hold, a group past the groups with names, and an archive that the tools
// didn't lay out.
format::File AddWiiSequenceSound(const format::File& file, std::optional<uint32_t> model, const std::string& name,
                                 const ntdlib::commands::Sequence& incoming, uint32_t start, const ProgramMap& programs,
                                 const std::optional<std::vector<uint32_t>>& banks = std::nullopt,
                                 std::span<const uint32_t> groups = {});

// The file after its archive of the Wii gains bank `name` with `bytes`, a bank file of the Wii, and its wave data
// `wave_data`, as a new file. A sound plays the new bank once its bank slot names it. Throws std::invalid_argument with
// a message for the user for a file that isn't of the Wii, a bank that ntdlib can't read with its wave data, a name
// that is empty or that the archive holds already, a group past the groups with names, and an archive that the tools
// didn't lay out.
format::File AddWiiBank(const format::File& file, const std::string& name, std::span<const uint8_t> bytes,
                        std::span<const uint8_t> wave_data, std::span<const uint32_t> groups = {});

// The file after its archive of the Wii gains `bytes`, a wave sound file of the Wii, and its wave data `wave_data`, as
// a new file, with a new sound for each of its wave sounds, named by WaveSoundNames(`name`). Each new sound copies
// sound `model` of the archive's sound table, a wave sound or a sequence sound: its player, its volume, its priorities,
// its 3D settings and its user parameters. Throws std::invalid_argument with a message for the user for a file that
// isn't of the Wii, a model that isn't a wave sound or a sequence sound, a wave sound file that ntdlib can't read with
// its wave data or that holds no wave sound, a name that is empty or that the archive holds already, a group past the
// groups with names, and an archive that the tools didn't lay out.
format::File AddWiiWaveSounds(const format::File& file, uint32_t model, const std::string& name,
                              std::span<const uint8_t> bytes, std::span<const uint8_t> wave_data,
                              std::span<const uint32_t> groups = {});

// The stream of `incoming`, a stream of the Wii (ReadIncomingWiiStreamFile and ReadIncomingWiiStreamWav), for a new
// stream sound named `name` that copies sound `model` of the Wii's `archive`. The path is stream/ and the name, then
// .brstm. A number follows the name while the archive's file table names the path in any case. The stream file gives
// the sound's tracks: `tracks_in_file` holds, and `details` stays empty. `file_tracks` holds for a stream file of the
// Wii and for a converted stream whose tracks differ from a track for each pair of its channels. A stream file of the
// Wii keeps its tracks as they are. A stream file of a later console without tracks takes such tracks. `wii_sound`
// takes the model's entry with the new sound's name, kind, file and allocations (AddWiiStreamSound). The Import window
// plays the new stream with it. Throws std::invalid_argument with a message for the user for a model past the sound
// table, a stream that ntdlib can't read, a stream without channels or tracks, and more than 8 tracks or 16 channels:
// NW4R SND's player of streams plays no more.
NewStream MakeNewWiiStream(const ntdlib::rsar::SoundArchive& archive, uint32_t model, const std::string& name,
                           const IncomingStream& incoming);

// The file after its archive of the Wii gains stream sound `name`, a copy of sound `model` of the archive's sound table
// that plays `stream` (MakeNewWiiStream) from a new file beside the archive, at `stream.path`. The new sound takes the
// model's player, volume, priority, pan mode and curve, 3D settings and user parameters. Its entry names every channel
// and track of the stream: the channels by their count from version 1.4 of the archive on, and by a bit for each
// before. The tools write such entries too. The new file lies in no group: a game plays a stream from the disc, a block
// at a time. The file keeps the stream as Hokuto's, and an export writes it beside the archive. Throws
// std::invalid_argument with a message for the user for a file that isn't of the Wii, a model past the sound table, a
// name that is empty or that the archive holds already, a path that the archive's file table names already, a stream
// that MakeNewWiiStream refuses, and an archive that the tools didn't lay out.
format::File AddWiiStreamSound(const format::File& file, uint32_t model, const std::string& name,
                               const NewStream& stream);

// Why the archive of `file`, a file of the Wii, takes no new item: the message of the edits for an archive that
// NintendoWare for Revolution's tools didn't lay out as Hokuto reads it. Nothing when it takes new items. The check
// exports the archive and reads its model.
std::optional<std::string> WiiAdditionsRefused(const format::File& file);

// The groups with names of `file`'s archive of the Wii, with the files that each one holds, in order, by their IDs. A
// group's index here is its index in the archive's group table. The archive's last group stays out. None for an archive
// that the tools didn't lay out.
std::vector<Group> WiiGroupsOf(const format::File& file);

} // namespace hokuto::edit
