// SPDX-License-Identifier: MIT

// The view of a sound that a file keeps (format::EditorView), worked out without a window: from the transport, the
// arrangement, the tracks' mutes and solos and the editors, and back into them. The file then opens as it was saved.
// The tests run without a window.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "app/key_editor.h"
#include "app/song_view.h"
#include "app/transport.h"
#include "app/views.h"
#include "format/file.h"

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

} // namespace hokuto::app
