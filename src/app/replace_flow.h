// SPDX-License-Identifier: MIT

// The state of Replace… and of the imports: what the file dialog returns a file for, the replacement that the Replace
// window shows, where the window opens, and the work of the window's Play and of cancelled imports.

#pragma once

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <vector>

#include "app/app_work.h"
#include "format/file.h"

namespace hokuto::app
{

struct ReplaceFlow
{
    // What the dialog of Replace… gives a file for: a sound's sequence, an item's file, a wave or a stream sound's
    // stream. Then the replacement that the Replace window shows.
    std::optional<uint32_t> sound;
    std::optional<FileTarget> file;
    std::optional<WaveTarget> wave;
    std::optional<uint32_t> stream;
    std::optional<Replacement> shown;

    // The centre of the Replace window where the user last dragged it. The window opens there until Hokuto quits.
    std::optional<ImVec2> place;

    // The top of the Replace window once it has shown. The window keeps it while it grows, and a new view of the same
    // window keeps it too. It is cleared when the window closes.
    std::optional<float> top;

    // The work of a cancelled import runs on to its end. It waits in `dropped_edits` until then.
    std::vector<std::future<EditWork>> dropped_edits;

    // The work of the Replace window's Play, until the preview starts. A dropped preview's work waits in
    // `dropped_previews` until it's done.
    std::optional<std::future<PreviewWork>> preview_work;
    std::vector<std::future<PreviewWork>> dropped_previews;

    // Whether the worker plays the Replace or Import window's Play.
    bool previewing = false;

    // Forgets what the dialog of Replace… was to give a file for (`sound` to `stream`).
    void ForgetTargets();

    // Ends the replacement or the import that the Replace window shows. The work of a confirmed import runs on until
    // it's done.
    void Drop();

    // Makes the sequence's replacement view again from the choices in the Replace window.
    void ChangeArchiveSequence();

    // The version and the target of PlayMapping for a new item of the DS, for a file in the place of another, and for a
    // new sequence sound that plays as its model would.
    void DsNewItemPreview(std::function<format::File()>& make, PreviewTarget& target);
    void ReplacedFilePreview(std::function<format::File()>& make, PreviewTarget& target);
    void ModelSequencePreview(std::function<format::File()>& make, PreviewTarget& target);

    // The edit that File > Import > Bank…, Wave Archive… or Wave Sound File… makes with the Import window's choices.
    // With `join`, the new items join the groups chosen in the window.
    std::function<format::File()> ItemImportEdit(bool join) const;

    // The edit that adds the banks of a SoundFont 2 file or an SFZ file. With `join`, the new banks join the groups
    // chosen in the window.
    std::function<format::File()> SoundFontEdit(bool join) const;

    // The edit that the Import window of a new item of the DS makes.
    std::function<format::File()> DsImportEdit() const;
};

} // namespace hokuto::app
