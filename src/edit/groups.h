// SPDX-License-Identifier: MIT

// The groups of a file's archive, for Replace… and File > Import. A game loads a group into its sound heap: the files
// whose copies the group's file holds, and the archive's files that it links to (ntdlib/sar/groups.h). A group loads an
// item when it holds the item's file: a sequence sound's sequence file, a wave sound's wave sound file, a bank's file
// or a wave archive's file. The windows propose the groups that load the item that a replacement changes, or the sound
// whose settings a new item copies. The edit puts the files that the item requires into the groups that the user
// chooses. The user can also name new groups for the item. They go at the end of the group table, each with an empty
// group's file (AddGroups in edit/new_items.h).
// A group that holds copies takes copies, and a group that links to the archive's files takes links. A group without
// files takes links, or copies when a link can't reach a file that it takes: a file that only groups hold. A group that
// the game keeps beside the archive takes them in its group file. The Hokuto file has to hold that group file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"

#include "format/file.h"

namespace hokuto::edit
{

// What a replacement changes in an item: the bytes of the item's new file, or a sequence sound's new bank slots, as
// bank item IDs. An empty `file` leaves the item's file as it is.
struct ItemChange
{
    std::span<const uint8_t> file;
    std::optional<std::vector<uint32_t>> banks;
};

// The files that item `item` of `archive` requires in a group that loads it, by their file IDs, in order, each once.
// The item's file comes first. A sequence sound requires its sequence file and the files that the banks of its slots
// require. A wave sound requires its wave sound file and the wave archives that the file's waves name. A bank requires
// its file, the wave archives that its waves name and those that its entry lists. A wave archive requires its file. A
// stream sound requires no file in a group. Neither does an item past its table. A file that ntdlib can't read names no
// wave archives. With `change`, the item has the file or the bank slots that a replacement gives it.
std::vector<uint32_t> FilesOfItem(const ntdlib::sar::SoundArchive& archive, uint32_t item,
                                  const ItemChange& change = {});

// The files of `files` whose bytes `archive` lacks, by their IDs, such as a file that only a group file holds when the
// Hokuto file lacks that group file. No group can take them (JoinGroups).
std::vector<uint32_t> UnreadableFiles(const ntdlib::sar::SoundArchive& archive, std::span<const uint32_t> files);

// A group of a file's archive: its index in the archive's group table, its name, and the files that it holds, by their
// IDs. A group that the edit can't reach says why in `unavailable`.
struct Group
{
    friend bool operator==(const Group&, const Group&) = default;

    uint32_t group = 0;
    std::string name;
    std::vector<uint32_t> files;
    std::string unavailable;
};

// The groups of `archive`, the archive that `file` builds with its group files. ntdlib reads them and stops at the
// first problem. None when ntdlib finds a problem in the archive's tables. A group that the game keeps beside the
// archive is unavailable when the Hokuto file lacks its group file. So is a group whose file ntdlib can't read.
std::vector<Group> GroupsOf(const format::File& file, const ntdlib::sar::SoundArchive& archive);

// A group that the windows list: its index in the archive's group table and its name, whether it holds the item
// already, and whether the window proposes it. A group that the edit can't reach says why in `unavailable`.
struct GroupOption
{
    friend bool operator==(const GroupOption&, const GroupOption&) = default;

    uint32_t group = 0;
    std::string name;
    bool holds = false;
    bool proposed = false;
    std::string unavailable;
    std::vector<uint32_t> taken;
};

// The options of `groups` for an item that requires the files `after` (FilesOfItem) in the place of an item that
// required `before`. For a new item, `after` lists the files that the new item requires, with its new file first under
// an ID past the archive's files, or nothing. `before` then lists the files of the sound whose settings the new item
// copies, if any. A group loads the old item or the model when it holds the first file of `before`. A group holds the
// item when it would take none of `after`: a group that loads the old item takes only the files that the old item
// didn't require (JoinGroups). No group holds a new item. `taken` lists the files that a group would take, in the order
// of `after`. A group that would take a file of `unreadable` (UnreadableFiles) is unavailable: the edit would refuse
// it. The window proposes each group that loads the old item or the model and doesn't hold the item, unless the edit
// can't reach the group.
std::vector<GroupOption> GroupOptions(std::span<const Group> groups, std::span<const uint32_t> before,
                                      std::span<const uint32_t> after, std::span<const uint32_t> unreadable = {});

// The file after groups `groups` of its archive load `items`. `before` lists the files that the old item required
// before a replacement, or the files of the sound whose settings new items copy (FilesOfItem), by their IDs in `file`.
// Each group takes the files that the items require (FilesOfItem) and that it lacks. A group that loads the old item or
// the model takes only those that `before` lacks: the game loads the others from elsewhere. A group lists each item
// whose file it didn't hold before, unless it lists the item already. The load flags name every part when the group
// then holds every file of the item, or else the parts all of whose files it holds. The groups' files and the files'
// lists of groups follow (ntdlib::sar::WithGroupMembers, GroupWith and WithGroupLists). The file's other parts stay
// (Rebuilt). New groups named `new_groups` go in first, after the archive's other groups (AddGroups). They load the
// items too. Throws std::invalid_argument with a message for the user for a group past the group table, a group whose
// group file the Hokuto file lacks, a link to a file that only groups hold, a file that Hokuto can't read, and a
// group's file laid out otherwise than NintendoWare's tools lay one out. Throws AddGroups's exceptions too.
format::File JoinGroups(const format::File& file, std::span<const uint32_t> groups, std::span<const uint32_t> items,
                        std::span<const uint32_t> before, std::span<const std::string> new_groups = {});

} // namespace hokuto::edit
