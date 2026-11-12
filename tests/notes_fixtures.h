// SPDX-License-Identifier: MIT

// This header contains the archives and helpers that the tests of the note edits share.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/driver.h"
#include "ntdlib/mp2k/song.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/testing/dse_song_files.h"
#include "ntdlib/testing/mp2k_cart.h"
#include "ntdlib/testing/nds_files.h"
#include "ntdlib/testing/sar_files.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/testing/check.h"

#include "edit/commands.h"
#include "edit/instruments.h"
#include "edit/mix.h"
#include "edit/notes.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/sequence.h"

using hokuto::edit::AddSettings;
using hokuto::edit::CommandKey;
using hokuto::edit::CommandsAfter;
using hokuto::edit::Edited;
using hokuto::edit::EditNotes;
using hokuto::edit::LimitsOf;
using hokuto::edit::MoveSettings;
using hokuto::edit::NewNote;
using hokuto::edit::NewSetting;
using hokuto::edit::NoteEdit;
using hokuto::edit::NoteLimits;
using hokuto::edit::NotePlace;
using hokuto::edit::SettingMove;
using hokuto::edit::ShiftRange;
using hokuto::edit::ShiftRangeOf;
using hokuto::edit::UnstartedPlaces;
using hokuto::edit::WhyFixedInTime;
using hokuto::format::File;
using hokuto::song::Note;
using hokuto::song::Sequence;
using hokuto::song::Setting;
using hokuto::song::Stretch;
using ntdlib::commands::Kind;
using ntdlib::sequence_export::Entry;
using xsflib::testing::ErrorOf;

namespace
{

namespace sar = ntdlib::sar;
namespace sar_files = ntdlib::testing::sar_files;

// The start, length and key of each note of track 0 of `sequence`.
inline std::vector<std::vector<uint32_t>> Notes(const Sequence& sequence)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const Note& note : sequence.tracks.at(0).notes)
    {
        notes.push_back({note.start, note.length, note.key});
    }

    return notes;
}

// An edit of the note at `index` of track 0 of `sequence`, in the stretch that ran it.
inline NoteEdit EditOf(const Sequence& sequence, std::size_t index)
{
    const Note& note = sequence.tracks.at(0).notes.at(index);
    return NoteEdit{.command = note.command, .stretch = sequence.tracks[0].stretches.at(note.stretch)};
}

// A command that sets the volume to `volume`.
inline ntdlib::commands::Command Volume(int volume)
{
    ntdlib::commands::Command command;
    command.kind = Kind::kVolume;
    command.value = ntdlib::commands::Argument{ntdlib::commands::Source::kValue, volume * ntdlib::commands::kOne, 0};

    return command;
}

// Whether `edit` refuses with a message for the user.
inline bool Refuses(const File& file, uint32_t sequence, const NoteEdit& edit)
{
    return !ErrorOf<std::invalid_argument>([&] { EditNotes(file, sequence, std::vector<NoteEdit>{edit}); }).empty();
}

// The start, length, key and velocity of each note of track `track` of `sequence`.
inline std::vector<std::vector<uint32_t>> GbaNotes(const Sequence& sequence, std::size_t track)
{
    std::vector<std::vector<uint32_t>> notes;
    for (const Note& note : sequence.tracks.at(track).notes)
    {
        notes.push_back({note.start, note.length, note.key, static_cast<uint32_t>(note.velocity)});
    }

    return notes;
}

} // namespace
