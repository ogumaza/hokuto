// SPDX-License-Identifier: MIT

// The views of the Replace window for a sequence on the 3DS, the Wii U, the Switch and the Wii, and the MIDI maps
// of their banks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "app/replace_view.h"
#include "edit/replace.h"
#include "edit/variables.h"
#include "format/file.h"

namespace hokuto::app
{

// Fills `source.groups` (edit::GroupsOf, or edit::WiiGroupsOf on the Wii) once the source holds its file and its
// archive. The window reads the groups once, and its views offer them. It also measures the archive that an export
// writes: `source.file_sizes` and `source.group_sizes` (format::SizesOf). The export folds each timeline into the
// converter's subroutines (edit::FoldTimelines). The archive that the program plays holds the timelines with every bar
// in their tracks, and their files take more bytes there. The window's sizes then follow the Export window's. On the
// Wii, FindGroups returns why the archive accepts no new item (edit::WiiAdditionsOf). One build of the archive finds
// the groups, the sizes and that reason. Elsewhere FindGroups returns nothing.
std::optional<std::string> FindGroups(ReplaceSource& source);

// Fills `source.sound_starts` (edit::SoundStarts) once the source holds its file, its places and the incoming sequence
// file. A file without the places gives no starts. A timeline's commands have to be laid out with its subroutines for
// the comparison. The window therefore keeps the starts.
void FindSoundStarts(ReplaceSource& source);

// Fills `source.readers` (edit::VariableReaders) once the source holds its file and its places. A file without the
// places gives no readers.
void FindReaders(ReplaceSource& source);

// The sequences whose commands a replacement of the sequence of `source.sound` replaces with the file's commands, for
// edit::SettleTimelines: the sound's sequence when no other sound plays its file. Otherwise the new commands go after
// the file's commands, and the other sounds keep theirs.
std::vector<uint32_t> ReplacedSequences(const ReplaceSource& source);

// The pairings that the file's maps contain for `source.midi`, a MIDI file, with the banks that its sound plays. The
// source gets the pairings of the drum notes, and the new commands that they make. Returns the pairings of the
// programs.
edit::ProgramMap ProposeMidiMap(ReplaceSource& source);

// Updates the commands of `source` from its MIDI file with the pairings of the drum notes in `view`, once a row
// changes.
void FollowDrumRows(ReplaceSource& source, const ReplaceView& view);

// The maps that a file contains once the import of `view` keeps the pairings of its rows, by the indexes of the banks.
// `banks` names the bank in each slot. Where it's empty, the banks come from `source.banks`, or else from the slots of
// the sound in the archive of the Wii or of a later console (SoundBanks). A source of the DS or of DSE therefore passes
// `banks`. For a source without a MIDI file, returns none.
std::map<uint32_t, format::BankMidiMap> MidiMapsOf(const ReplaceView& view, const ReplaceSource& source,
                                                   const std::vector<uint32_t>& banks);

// `file` with `maps` in the place of its maps of those banks.
void KeepMidiMaps(format::File& file, const std::map<uint32_t, format::BankMidiMap>& maps);

// The view of `source`'s replacement from start `chosen`, with `programs` in the place of the programs that the new
// commands select, and the moves and fixed values of `variables`. The sound allocates the tracks that the new commands
// open and its old commands don't. With `all_tracks`, it allocates every track that the new commands open. The sound
// takes the bank slots of `source.banks` if the user chose them. Without a choice, the window starts where the sound
// named after the file starts, or else at the label named after the file, or else at the first label, or else at the
// file's first command. The window proposes the groups that load the sound and lack a file of its new bank slots.
ReplaceView MakeReplaceView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                            const edit::ProgramMap& programs, const edit::VariableMap& variables, bool all_tracks);

// The view of `source`'s sequence as a new sound named `name`, a copy of sound `source.sound`, from start `chosen`,
// with `programs` in the place of the programs that the new commands select, and the moves and fixed values of
// `variables` (File > Import > Sequence…). The view holds no result. Without a choice, the window starts where the
// sound named after the file starts, or else at the label named after the file, or else at the first label, or else at
// the file's first command. The window proposes the groups that load the sound that the new one copies.
ReplaceView MakeImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                           const edit::ProgramMap& programs, const edit::VariableMap& variables,
                           const std::string& name);

// The name that the Import window offers first for the file `file_name`: the file's name without its extension, with a
// number after it while an item of the archive holds the name or the browser shows it. `file` holds the archive, and
// `archive` is the archive that `file` builds. Hokuto's Export… names a file after its item. For the `count` sounds of
// a wave sound file, none of the names that edit::WaveSoundNames makes from it may be taken. An archive without names
// gets an empty name: the edits refuse a name there.
std::string FirstName(const format::File& file, const ntdlib::sar::SoundArchive& archive, const std::string& file_name,
                      std::size_t count = 1);

// The name that the Import window offers first for the file `file_name` on the Wii: the file's name without its
// extension, with a number after it while an item of `source.played.Wii()` or one of its groups holds the name.
// FirstName follows the same rule.
std::string FirstWiiName(const ReplaceSource& source, const std::string& file_name, std::size_t count = 1);

// The view of `source`'s sequence as a new sound of `source.played.Wii()`. MakeImportView makes the same view for the
// other consoles. The sound has one bank slot. The window proposes the groups with names that load the sound that the
// new one copies. Without a model, the new sound takes Hokuto's settings (edit::AddWiiSequenceSound). Play waits for a
// preview of the Wii's new items.
ReplaceView MakeWiiImportView(const ReplaceSource& source, std::optional<std::size_t> chosen,
                              const edit::ProgramMap& programs, const edit::VariableMap& variables,
                              const std::string& name);

} // namespace hokuto::app
