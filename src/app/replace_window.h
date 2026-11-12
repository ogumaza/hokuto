// SPDX-License-Identifier: MIT

// The Replace window of the browser's Replace…: a sequence file in the place of a sound's sequence (edit/replace.h), a
// bank, a wave archive or a wave sound file in the place of another, a WAV file in the place of a wave (edit/files.h),
// or a stream file or a WAV file in the place of a stream sound's stream (edit/streams.h). For a sequence, the window
// shows the file and offers its starts and the banks of the sound's slots. It pairs each program that the new commands
// select with a program of the bank in the same slot. Each variable that the new commands read before they set it keeps
// its number, moves to another or takes a fixed value (edit/variables.h). For a bank or a wave sound file, it pairs
// each wave archive that the file's waves name with a wave archive of the archive. It lists the warnings before
// anything changes. The same window serves File > Import > Sequence…, Bank…, Wave Archive…, Wave Sound File… and
// Stream… (edit/new_items.h). It then names the new item. For a sequence, a wave sound file or a stream, it offers the
// sounds whose settings the new sounds can copy. In a file of the GBA, File > Import > Sequence… imports a MIDI file in
// the place of the commands of a song (edit/gba_midi.h). The window offers the songs, and pairs each instrument that
// the file's notes play with a voice of the song's voice group. On DSE, a song file or a MIDI file replaces a sound, a
// wave bank file a bank, and a WAV file a wave (edit/dse_files.h).

#pragma once

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sar/formats.h"

#include "app/replace_driver_views.h"
#include "app/replace_ds_views.h"
#include "app/replace_file_views.h"
#include "app/replace_sequence_views.h"
#include "app/replace_stream_views.h"
#include "app/replace_view.h"
#include "app/replace_wave_views.h"
#include "edit/ds_new_items.h"
#include "edit/dse_files.h"
#include "edit/files.h"
#include "edit/gba_midi.h"
#include "edit/gba_sound_fonts.h"
#include "edit/groups.h"
#include "edit/midi_songs.h"
#include "edit/new_items.h"
#include "edit/replace.h"
#include "edit/sound_font_imports.h"
#include "edit/streams.h"
#include "edit/variables.h"
#include "format/file.h"
#include "song/ds.h"
#include "song/dse.h"
#include "song/gba.h"
#include "song/sequence.h"

namespace hokuto::app
{

// Draws the Replace window while a replacement waits. `place` keeps the window's centre where the user dragged it.
// Returns what the user did in this frame.
// `top` keeps the top of the window once it has shown.
ReplaceChoice DrawReplaceWindow(ReplaceView& view, std::optional<ImVec2>& place, std::optional<float>& top);

// Closes the Replace window if it's open.
void CloseReplaceWindow();

} // namespace hokuto::app
