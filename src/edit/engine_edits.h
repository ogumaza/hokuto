// SPDX-License-Identifier: MIT

// The edits that differ from one family of engines to another (format::Family). The functions of the command layer,
// such as AddTrack, pass each edit through EditsOf to the class for the family of the file. Each of the other GBA
// drivers has a separate class (edit/drivers.h). A new family adds a class in engine_edits.cpp and a case in EditsOf.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ntdlib/commands/sequence.h"

#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/notes.h"
#include "edit/replace.h"
#include "edit/tracks.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/sequence.h"

namespace hokuto::edit
{

// The edits of one family of engines. Each method takes a file of the family. A method throws std::invalid_argument
// with a message for the user when the format of the family can't store the edit.
class EngineEdits
{
public:
    virtual ~EngineEdits() = default;

    // The Project menu's edits of tracks: AddTrack, DuplicateTrack, CopyTrack, PasteTrack and RemoveTrack
    // (edit/tracks.h). `song` gives the sound as ntdlib's performer played it, and `instrument` the instrument that a
    // new track selects first, where the family's tracks select one.
    virtual TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                               uint32_t sound, const song::Sequence& song, std::optional<Program> instrument) const = 0;
    virtual TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                     uint32_t sound, int track) const = 0;
    virtual CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                  uint32_t sound, const song::Sequence& song, int track) const = 0;
    virtual TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                 uint32_t sound, const song::Sequence& song, const CopiedTrack& copied) const = 0;
    virtual format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                                     uint32_t sound, int track) const = 0;

    // The edits of notes and settings (edit/notes.h). Writable returns `edited` as the bytes of the family can store
    // it. `edited` is an edit of sequence `sequence` of `before`, and `Edited::items` and `Edited::added` follow the
    // new items. Placed returns `edited`, an edit of the notes or the settings, with what else the bytes keep around
    // such an edit. Writable comes after it. EditNotes is edit::EditNotes on the family, Writable included.
    // NoteWaitOpens says whether the tracks of `sequence` open with the note wait on. Their notes then wait for their
    // lengths. The defaults keep `edited` as it is, edit the notes by EditNotesWithLengths, and open with the note wait
    // on.
    virtual Edited Writable(const format::File& before, Edited edited, uint32_t sequence) const;
    virtual Edited Placed(const format::File& before, Edited edited, uint32_t sequence) const;
    virtual Edited EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                             std::span<const NewNote> additions) const;
    virtual bool NoteWaitOpens(const ntdlib::commands::Sequence& sequence) const;

    // The openings of tracks (InsertAtOpening and RunsInto). OpensTrack says whether `item`, an item of `sequence`,
    // opens track `track` of a sound that plays the sequence. JumpsPastOpenings says whether new commands at the
    // opening of a track take a jump past them where the item before them runs into them. IsTableEntry says whether
    // `item` is an entry of a table of tracks rather than a command. StartedTracksTakeSettings says whether a track
    // that a command of another track starts copies the settings of that track. The defaults open a track other than
    // track 0 at its kOpenTrack command, take the jump, and give no entries and no settings.
    virtual bool OpensTrack(const ntdlib::commands::Sequence& sequence, const ntdlib::commands::Command& item,
                            int track) const;
    virtual bool JumpsPastOpenings() const;
    virtual bool IsTableEntry(const ntdlib::commands::Command& item) const;
    virtual bool StartedTracksTakeSettings() const;

    // ArgumentRange (edit/commands.h) on the family, for a command that track `track` of `console` runs.
    virtual std::optional<ValueRange> Range(const song::Console& console, int track,
                                            const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                            bool time) const = 0;

    // CommandsWithoutWayOut (edit/bytecode.h) on the family. `entries` contains the items where sounds start.
    virtual std::size_t LoopsWithoutWaits(const ntdlib::commands::Sequence& sequence,
                                          std::span<const uint32_t> entries) const = 0;
};

// The edits for the engine family of `console`: the driver class for one of the other GBA drivers, and otherwise the
// class of the engine family.
const EngineEdits& EditsOf(const song::Console& console);

} // namespace hokuto::edit
