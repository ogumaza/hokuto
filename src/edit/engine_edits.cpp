// SPDX-License-Identifier: MIT

// The classes of the families of engines (edit/engine_edits.h), and the choice of one for a console.

#include "edit/engine_edits.h"

#include <utility>

#include "edit/drivers.h"
#include "edit/engine_loops.h"
#include "edit/engine_ranges.h"
#include "edit/engine_tracks.h"
#include "edit/mp2k.h"
#include "format/engines.h"

namespace hokuto::edit
{
namespace
{

// The sound archives of the later consoles, the Wii and NITRO-Composer. These consoles share one body for each track
// edit. That body tests for the DS where the DS differs.
class ArchiveEdits : public EngineEdits
{
public:
    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence& song, std::optional<Program> instrument) const override
    {
        return track_detail::AddArchiveTrack(file, places, sound, song, instrument);
    }

    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return track_detail::DuplicateArchiveTrack(file, places, sound, track);
    }

    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override
    {
        return track_detail::CopyArchiveTrack(file, places, sound, song, track);
    }

    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, const CopiedTrack& copied) const override
    {
        return track_detail::PasteArchiveTrack(file, places, sound, song, copied);
    }

    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return track_detail::RemoveArchiveTrack(file, places, sound, track);
    }

    std::optional<ValueRange> Range(const song::Console& console, int /*track*/,
                                    const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                    bool time) const override
    {
        return range_detail::ArchiveRange(console, command, source, time);
    }

    std::size_t LoopsWithoutWaits(const ntdlib::commands::Sequence& sequence,
                                  std::span<const uint32_t> entries) const override
    {
        return loop_detail::CommandsWithoutWayOutByJumps(sequence, entries, false);
    }
};

// NITRO-Composer on the DS. Its track edits are those of ArchiveEdits.
class NitroComposerEdits final : public ArchiveEdits
{
public:
    std::optional<ValueRange> Range(const song::Console& /*console*/, int /*track*/,
                                    const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                    bool time) const override
    {
        return range_detail::NtrRange(command, source, time);
    }

    std::size_t LoopsWithoutWaits(const ntdlib::commands::Sequence& sequence,
                                  std::span<const uint32_t> entries) const override
    {
        return loop_detail::CommandsWithoutWayOutByJumps(sequence, entries, true);
    }
};

// MusicPlayer2000 on the GBA.
class Mp2kEdits final : public EngineEdits
{
public:
    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence& song, std::optional<Program> instrument) const override
    {
        return track_detail::AddGbaTrack(file, places, sound, song, instrument);
    }

    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return track_detail::DuplicateGbaTrack(file, places, sound, track);
    }

    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override
    {
        return track_detail::CopyGbaTrack(file, places, sound, song, track);
    }

    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, const CopiedTrack& copied) const override
    {
        return track_detail::PasteGbaTrack(file, places, sound, song, copied);
    }

    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return track_detail::RemoveGbaTrack(file, places, sound, track);
    }

    Edited Writable(const format::File& /*before*/, Edited edited, uint32_t sequence) const override
    {
        return Mp2kWritable(std::move(edited), sequence);
    }

    std::optional<ValueRange> Range(const song::Console& console, int /*track*/,
                                    const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                    bool time) const override
    {
        return range_detail::Mp2kRange(command, source, time, console.note_arguments);
    }

    std::size_t LoopsWithoutWaits(const ntdlib::commands::Sequence& sequence,
                                  std::span<const uint32_t> entries) const override
    {
        return loop_detail::CommandsWithoutWayOutByJumps(sequence, entries, false);
    }

    // The notes of MusicPlayer2000 never wait.
    bool NoteWaitOpens(const ntdlib::commands::Sequence& /*sequence*/) const override
    {
        return false;
    }
};

// Procyon Studio's DSE on the DS.
class DseEdits final : public EngineEdits
{
public:
    TrackEdit AddTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                       const song::Sequence& song, std::optional<Program> instrument) const override
    {
        return track_detail::AddDseTrack(file, places, sound, song, instrument);
    }

    TrackEdit DuplicateTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return track_detail::DuplicateDseTrack(file, places, sound, track);
    }

    CopiedTrack CopyTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                          const song::Sequence& song, int track) const override
    {
        return track_detail::CopyDseTrack(file, places, sound, song, track);
    }

    TrackEdit PasteTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places, uint32_t sound,
                         const song::Sequence& song, const CopiedTrack& copied) const override
    {
        return track_detail::PasteDseTrack(file, places, sound, song, copied);
    }

    format::File RemoveTrack(const format::File& file, std::span<const std::optional<SoundPlace>> places,
                             uint32_t sound, int track) const override
    {
        return track_detail::RemoveDseTrack(file, places, sound, track);
    }

    std::optional<ValueRange> Range(const song::Console& /*console*/, int /*track*/,
                                    const ntdlib::commands::Command& command, ntdlib::commands::Source source,
                                    bool time) const override
    {
        return range_detail::DseRange(command, source, time);
    }

    std::size_t LoopsWithoutWaits(const ntdlib::commands::Sequence& sequence,
                                  std::span<const uint32_t> /*entries*/) const override
    {
        return loop_detail::DseCommandsWithoutWayOut(sequence);
    }

    // The notes of DSE never wait.
    bool NoteWaitOpens(const ntdlib::commands::Sequence& /*sequence*/) const override
    {
        return false;
    }
};

} // namespace

Edited EngineEdits::Writable(const format::File& /*before*/, Edited edited, uint32_t /*sequence*/) const
{
    return edited;
}

Edited EngineEdits::Placed(const format::File& /*before*/, Edited edited, uint32_t /*sequence*/) const
{
    return edited;
}

Edited EngineEdits::EditNotes(const format::File& file, uint32_t sequence, std::span<const NoteEdit> edits,
                              std::span<const NewNote> additions) const
{
    return EditNotesWithLengths(file, sequence, edits, additions);
}

bool EngineEdits::NoteWaitOpens(const ntdlib::commands::Sequence& /*sequence*/) const
{
    return true;
}

bool EngineEdits::OpensTrack(const ntdlib::commands::Sequence& /*sequence*/, const ntdlib::commands::Command& item,
                             int track) const
{
    return track != 0 && item.kind == ntdlib::commands::Kind::kOpenTrack && item.index == track;
}

bool EngineEdits::JumpsPastOpenings() const
{
    return true;
}

bool EngineEdits::IsTableEntry(const ntdlib::commands::Command& /*item*/) const
{
    return false;
}

bool EngineEdits::StartedTracksTakeSettings() const
{
    return false;
}

const EngineEdits& EditsOf(const song::Console& console)
{
    static const ArchiveEdits kArchive;
    static const NitroComposerEdits kNitroComposer;
    static const Mp2kEdits kMp2k;
    static const DseEdits kDse;
    if (console.driver != nullptr)
    {
        return EditsOf(*console.driver);
    }

    // A console with a driver engine but no driver rules uses ArchiveEdits.
    switch (format::FactsOf(console.engine).family)
    {
    case format::Family::kMp2k:
        return kMp2k;
    case format::Family::kDse:
        return kDse;
    case format::Family::kNitroComposer:
        return kNitroComposer;
    case format::Family::kNintendoWare:
    case format::Family::kNw4r:
    case format::Family::kGbaDriver:
        break;
    }

    return kArchive;
}

} // namespace hokuto::edit
