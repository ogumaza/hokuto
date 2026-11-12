// SPDX-License-Identifier: MIT

// The view of a sound that a file keeps (format::EditorView), worked out without a window: from the transport, the
// arrangement, the tracks' mutes and solos and the editors, and back into them. The file then opens as it was saved.
// It also works out the cycle's locators for a sound's sequence, and after a new version of the sequence. The tests run
// without a window.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "app/key_editor.h"
#include "app/song_view.h"
#include "app/transport.h"
#include "app/views.h"
#include "format/file.h"
#include "song/sequence.h"

namespace hokuto::app
{

// The tracks that the editors show while they're open, by their numbers in the sound's sequence, and the window in
// front of the zone that the mixer and the editors share.
struct OpenEditors
{
    friend bool operator==(const OpenEditors&, const OpenEditors&) = default;

    std::optional<int> key_editor;
    std::optional<int> list_editor;
    format::ViewFront front = format::ViewFront::kMixer;
};

// The view of sound `sound` that the program shows. `numbers` gives the number of each track of `song`, in order. A set
// of the view's tracks holds the tracks numbered up to format::kLastViewTrack. So do the view's selected track and its
// editors' tracks.
format::EditorView ViewOf(uint32_t sound, std::span<const int> numbers, const Transport& transport,
                          const ArrangementState& arrangement, const SongView& song, const KeyEditorState& key_editor,
                          const OpenEditors& editors);

// Puts `view` back: the transport's position and cycle, the arrangement's zoom and scroll, its selected track and the
// mutes and solos of the tracks of `song`, whose numbers `numbers` gives, and the Key Editor's zoom, scroll and lanes.
// A position that a float rounded goes back to the tick of the sound or of the display that it stood at. The
// arrangement and the Key Editor take the scroll in their next frames. A lane of a setting that the lanes' menu doesn't
// offer (LaneGroups) stays out. Returns the tracks that the editors open on, of those that `numbers` holds, and the
// window in front. An editor in front that doesn't open gives way to the List Editor, then the Key Editor, then the
// mixer.
OpenEditors ApplyView(const format::EditorView& view, std::span<const int> numbers, Transport& transport,
                      ArrangementState& arrangement, SongView& song, KeyEditorState& key_editor);

// The cycle's locators, in quarter notes (Transport::CycleStart and Transport::CycleEnd).
struct Locators
{
    friend bool operator==(const Locators&, const Locators&) = default;

    double start = 0.0;
    double end = 0.0;
};

// The locators that showing a sound's sequence sets: round its loop, or else from its start to the beat on which its
// last note has ended, or else over its length. The release after the last note still plays, but a cycle leaves it
// out.
Locators SongLocators(const song::Sequence& sequence);

// The locators after `next`, a new version of a sound's sequence, replaces `shown`, such as after Replace…, an import
// into the sound, a move of its loop or an edit of its notes. Locators at `now` that showing `shown` set (SongLocators)
// follow the new version. Where `shown` has a loop, such locators fall on its ticks, in the timebase of `shown`.
// Without a loop, they start at the sequence's start and end at or past the beat on which its last note ends: they may
// have grown. Such locators go round the loop of `next`. Where `next` has no loop, they only grow. They reach the beat
// on which its last note ends once that beat passes them, and stay where it moves back. Other locators stay where the
// user set them, unless they start at or past the end of `next`. Its playback would never reach the cycle. They then go
// where showing `next` sets them. Locators move only by the user or for a clear reason, such as a song that grows past
// the cycle.
Locators LocatorsAfter(const Locators& now, const song::Sequence& shown, const song::Sequence& next);

} // namespace hokuto::app
