// SPDX-License-Identifier: MIT

// The views of the Replace window for a bank, a wave archive, a wave sound file or a SoundFont on the 3DS, the
// Wii U, the Switch and the Wii.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/replace_view.h"
#include "edit/files.h"
#include "edit/new_items.h"
#include "edit/sound_font_imports.h"
#include "format/file.h"

namespace hokuto::app
{

// The view of `incoming`, a bank file, a wave archive file or a wave sound file of the archive's console, as a new item
// named `name` (File > Import > Bank…, Wave Archive… or Wave Sound File…). The rows of a bank or a wave sound file pair
// each wave archive that `named` lists with the wave archive of `targets`, by its place in the archive's list, or -1
// for none. A wave sound file's new sounds copy the settings of sound `source.sound`, one of `source.models`. The view
// holds no result. For a wave sound file, the window proposes the groups that load that sound. No sound plays a new
// bank or wave archive, and the window proposes no group for it. With `source.new_bank`, the view is Project > Add
// Bank…'s, and `incoming` an empty bank.
ReplaceView MakeItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                               const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                               const std::string& name);

// The view of `import`, the import of a SoundFont as new banks named `name` (edit::SoundFontBankNames): File > Import >
// Bank… on a SoundFont 2 file. The window lists what the new banks leave out. It proposes no group: no sound plays a
// new bank yet.
ReplaceView MakeSoundFontImportView(const ReplaceSource& source, const edit::SoundFontImport& import,
                                    const std::string& name);

// The wave archives that the Import window offers first for the waves of `file`, a bank file or a wave sound file, one
// for each that `named` lists. Where a bank or a wave sound of the archive has the same file, they're the wave archives
// that the file names. So are they for a file in the place of another when `present`, the wave archives that the
// present file names, lists each of them. Otherwise they're the archive's last wave archives, in order: the wave
// archives imported last.
std::vector<int> FirstWaveArchives(const ntdlib::sar::SoundArchive& archive, std::span<const uint8_t> file,
                                   const std::vector<edit::NamedWaveArchive>& named,
                                   const std::vector<edit::NamedWaveArchive>& present = {});

// The view of a file in the place of item `source.sound` of `source.played.Sar()`, a bank, a wave archive or a wave
// sound by the kind of `incoming`. The rows of a bank or a wave sound file pair each wave archive that `named` lists
// with the wave archive of `targets`, by its place in the archive's list, or -1 for none. `incoming` names the chosen
// wave archives (edit::WithWaveArchiveMap), and holds the instruments of `fixes` (edit::WithProgramFixes). `changes`
// gives what the new file changes besides the item, without the fixes, and `result` the file that the replacement
// makes. Without a result, `error` says why the file can't go in. A bank's rows give each program that the sounds
// select and the new bank lacks one of `fixes`, or none. `source.places` gives where the sounds play. Without the
// places, a warning lists the present bank's programs that the new one lacks. The window proposes the groups that load
// the item and lack a file that the new file requires, such as a wave archive that its waves name.
ReplaceView MakeFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                const edit::FileChanges& changes, std::optional<format::File> result, std::string error,
                                const std::vector<edit::NamedWaveArchive>& named = {},
                                const std::vector<int>& targets = {}, std::span<const edit::ProgramFix> fixes = {});

// The same for a file of the Wii in the place of item `source.sound` of `source.played.Wii()`: a bank, the wave data of
// a bank or a wave sound file (edit::FileIdOf), or a wave sound's file, by the kind of `incoming`. The window offers no
// groups: the file keeps its places in them. For a bank file or a wave sound file of another console (`source.later`),
// the rows pair each wave archive that `named` lists with the wave data of `targets`, by its place among
// song::WaveArchivesOf, or -1 for none. `incoming` is then the converted file (edit::WiiFileOf).
ReplaceView MakeWiiFileReplaceView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const edit::FileChanges& changes, std::optional<format::File> result,
                                   std::string error, const std::vector<edit::NamedWaveArchive>& named = {},
                                   const std::vector<int>& targets = {});

// The file that a new item of `incoming` brings into `source.played.Wii()`, with its wave data: a bank or a wave sound
// file of the Wii with the wave data chosen in the row of `targets` (edit::WiiFileWithWaves), a bank file or a wave
// sound file of another console converted with the rows' wave data (edit::WiiFileOf), or a wave archive's waves as the
// wave data of a new bank without instruments (edit::EmptyBank). The menu of the row of a file of the Wii lists the
// RWAR beside it first if `source.beside` holds one, then the archive's wave data in the order of song::WaveArchivesOf.
// A file that holds its wave data already, such as an empty bank, goes in as it is. Throws std::invalid_argument with a
// message for the user as those calls do.
edit::IncomingFile WiiImportedFile(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                   const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets);

// The wave data that the Import window offers first for `incoming` on the Wii, one for each row: for a file of the Wii,
// the RWAR beside it, or else the wave data of a file of the archive with the same bytes; for a file of another
// console, the wave data of file `present` where a replacement gives one, or else the archive's last wave data in
// order. -1 leaves a row without a choice.
std::vector<int> FirstWiiWaveData(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named,
                                  std::optional<uint32_t> present = std::nullopt);

// The view of `incoming` as a new item of `source.played.Wii()` named `name` (File > Import > Bank…, Wave Archive… or
// Wave Sound File…, and Project > Add Bank…). MakeItemImportView makes the same view for the other consoles. The rows
// choose the wave data of a bank or a wave sound file (WiiImportedFile). A wave sound file's new sounds copy the
// settings of sound `source.sound`. A wave archive goes in as the wave data of a new bank without instruments: the Wii
// keeps every wave in the wave data of a bank or a wave sound file. The view holds no result.
ReplaceView MakeWiiItemImportView(const ReplaceSource& source, const edit::IncomingFile& incoming,
                                  const std::vector<edit::NamedWaveArchive>& named, const std::vector<int>& targets,
                                  const std::string& name);

// The name of item `item` of the Wii's `archive` whose file is of `kind`: a bank's, a wave archive's by its file's ID,
// or a wave sound's.
std::string ItemName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item);

// The name of item `item` of `archive` whose file is of `kind`: a bank's, a wave archive's or a wave sound's.
std::string ItemName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item);

// The name that the browser lists for the item whose file Replace… replaces: ItemName's, but a wave sound file takes
// the name of its set of wave sounds (song::WaveSoundSets), such as WSDSET_SE. On the Wii, it takes WSD_FILE_ and its
// file ID.
std::string ReplacedName(const ntdlib::sar::SoundArchive& archive, edit::FileKind kind, uint32_t item);
std::string ReplacedName(const ntdlib::rsar::SoundArchive& archive, edit::FileKind kind, uint32_t item);

// The item ID of the item whose file a replacement of `kind` replaces: bank, wave archive or wave sound `item`, by its
// place in the archive's list.
uint32_t ReplacedItem(edit::FileKind kind, uint32_t item);

} // namespace hokuto::app
