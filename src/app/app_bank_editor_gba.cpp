// SPDX-License-Identifier: MIT

// The Bank Editor on the GBA: the voice groups of MusicPlayer2000 and the instruments of the other GBA drivers, their
// edits, and the files dropped on a voice group.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/mp2k/song.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "edit/gba_voices.h"
#include "edit/konami_samples.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/gba_samples.h"
#include "song/konami.h"
#include "song/preview.h"

namespace hokuto::app
{
namespace
{

// Updates `view` after `bank_edit`, an edit of `voice`.
void ShowGbaVoiceEdit(GbaBankView& view, const BankEdit& bank_edit, const ntdlib::mp2k::Voice& voice)
{
    // The editor shows the new values at once, wherever the voice group plays the voice's bytes. The view follows the
    // version's ROM once the program has built it. A cleared voice waits for that.
    if (bank_edit.gba_cleared)
    {
        return;
    }
    if (bank_edit.gba_pattern)
    {
        view.patterns[bank_edit.gba_pattern->first] = bank_edit.gba_pattern->second;
        return;
    }
    if (bank_edit.gba_new_pattern)
    {
        view.patterns[voice.wave] = edit::NewGbaPattern();
    }
    const auto follow = [&voice](GbaVoiceRow& row)
    {
        if (row.voice.address == voice.address)
        {
            row.voice = voice;
            row.kind = song::GbaVoiceKind(voice);
        }
    };
    for (GbaProgram& program : view.programs)
    {
        follow(program.voice);
        std::ranges::for_each(program.rows, follow);
    }
}

} // namespace

void App::UpdateGbaBankEditor()
{
    const format::File* document = Document();
    if (!bank_editor_)
    {
        return;
    }
    if (document == nullptr || !archive_version_)
    {
        CloseBankEditor();
        return;
    }

    // The view follows each ROM that the program builds, and each move in the history back to the ROM that the program
    // has (FollowHistory). A version whose songs no longer play the voice group closes the editor. The editor follows a
    // table that moved.
    const auto follow = [this]
    {
        bank_editor_ = FollowedGbaGroup(gba_moved_groups_, *bank_editor_, GbaGroups());
        gba_bank_view_ = MakeGbaBankView(*played_.Gba(), *bank_editor_);
    };
    if (!FollowBankView(follow, "Can't edit this voice group. "))
    {
        return;
    }

    // As on the later consoles, files that waited for a field's text go on the voice group (UpdateBankEditor).
    TakeWaitingBankDrop();

    const bool busy = EditorsWait();
    bool open = true;
    const BankEdit bank_edit = DrawGbaBankEditor(*gba_bank_view_, bank_state_, ItemShown(bank_editor_), busy, &open);

    // The keyboard sounds its key on the selected program, with the music player, the priority and the reverb of the
    // first song that plays the voice group. Another key, or none, lets the last key go.
    const auto make = [&](std::pair<uint32_t, int> sounded,
                          std::shared_ptr<std::atomic<bool>> release) -> std::unique_ptr<audio::Renderer>
    {
        return std::make_unique<song::GbaNotePreview>(
            played_.Gba(), gba_bank_view_->song, static_cast<int>(sounded.first), sounded.second, bank_state_.velocity,
            release, edit::ModelMixing(document->mixing));
    };
    SoundBankNote(open && gba_bank_view_->song >= 0, make, "Can't play this voice. ");
    if (!open)
    {
        CloseBankEditor();
        return;
    }

    ApplyGbaBankEdit(bank_edit);
}

void App::ApplyGbaBankEdit(const BankEdit& bank_edit)
{
    if (!bank_edit.refused.empty())
    {
        ShowMessage(bank_edit.refused);
        return;
    }
    if (!bank_edit.gba_new.empty())
    {
        try
        {
            DoGbaGroupVersion(bank_edit.name, bank_edit.details, *bank_editor_,
                              edit::WithGbaVoicesAt(*Document(), *played_.Gba(), *bank_editor_, bank_edit.gba_new));
        }
        catch (const std::exception& error)
        {
            ShowMessage(error.what());
        }
        return;
    }
    if (bank_edit.gba_rows || bank_edit.gba_program_voice)
    {
        try
        {
            format::File file =
                bank_edit.gba_rows
                    ? edit::WithGbaKeyRows(*Document(), bank_edit.gba_rows->first, bank_edit.gba_rows->second)
                    : edit::WithGbaProgramVoice(*Document(), bank_edit.gba_program_voice->address,
                                                *bank_edit.gba_program_voice);
            Do(bank_edit.name, bank_edit.details, std::move(file));
        }
        catch (const std::exception& error)
        {
            ShowMessage(error.what());
        }
        return;
    }

    ApplyGbaVoiceEdit(bank_edit);
}

void App::ApplyGbaVoiceEdit(const BankEdit& bank_edit)
{
    if (!bank_edit.gba_voice && !bank_edit.gba_pattern && !bank_edit.gba_cleared)
    {
        return;
    }

    ntdlib::mp2k::Voice voice;
    if (bank_edit.gba_voice)
    {
        voice = *bank_edit.gba_voice;
    }
    try
    {
        format::File file = *Document();
        if (bank_edit.gba_new_pattern)
        {
            // The pattern of a new wave is written into the free space. The voice then names it.
            edit::PlacedGbaBytes placed = edit::WithGbaNewBytes(file, edit::GbaPatternBytes(edit::NewGbaPattern()));
            voice.wave = placed.address;
            file = std::move(placed.file);
        }
        if (bank_edit.gba_cleared)
        {
            file = edit::WithoutGbaVoice(file, *bank_edit.gba_cleared);
        }
        else if (bank_edit.gba_voice)
        {
            file = edit::WithGbaVoice(file, voice);
        }
        else
        {
            file = edit::WithGbaPattern(file, bank_edit.gba_pattern->first, bank_edit.gba_pattern->second);
        }
        const format::File* before = Document();
        Do(bank_edit.name, bank_edit.details, std::move(file));
        if (Document() == before || !gba_bank_view_)
        {
            return;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return;
    }

    ShowGbaVoiceEdit(*gba_bank_view_, bank_edit, voice);
}

void App::DoGbaGroupVersion(std::string name, std::string details, uint32_t from, edit::GbaGroupVersion version)
{
    const format::File* before = Document();
    const uint32_t group = version.group;
    if (group != from)
    {
        details += " (now " + song::GbaVoiceGroupName(group) + ")";
    }
    Do(std::move(name), std::move(details), std::move(version.file));
    if (Document() != before && group != from)
    {
        RecordGbaGroupMove(gba_moved_groups_, from, group);
    }
}

void App::DropFilesOnGbaBank(const std::vector<std::string>& paths, const BankDrop& drop)
{
    if (DropGbaAssembly(paths))
    {
        return;
    }

    const std::optional<std::vector<std::vector<uint8_t>>> read = ReadDroppedGbaSamples(paths);
    if (!read)
    {
        return;
    }

    const std::vector<std::vector<uint8_t>>& samples = *read;
    try
    {
        format::File file = *Document();
        const std::optional<ntdlib::mp2k::Voice> voice = DroppedGbaVoice(*gba_bank_view_, bank_state_, drop);
        if (voice)
        {
            if (samples.size() != 1)
            {
                ShowMessage(
                    "A voice plays only one sample. Drag one file onto the voice, or several onto the program list.");
                return;
            }
            edit::PlacedGbaBytes placed = edit::WithGbaNewBytes(file, samples.front());
            Do("Replace Sample", gba_bank_view_->name + ", program " + std::to_string(*drop.program),
               edit::WithGbaVoice(placed.file, edit::GbaVoiceOfKind(*voice, 0, placed.address)));
            return;
        }

        const std::vector<uint32_t>& free = gba_bank_view_->free_programs;
        if (free.size() < samples.size())
        {
            ShowMessage("Not enough free programs. The voice group has only " + std::to_string(free.size()) +
                        (free.size() == 1 ? " free program." : " free programs."));
            return;
        }
        std::vector<std::pair<uint32_t, ntdlib::mp2k::Voice>> voices;
        for (std::size_t i = 0; i < samples.size(); i++)
        {
            edit::PlacedGbaBytes placed = edit::WithGbaNewBytes(file, samples[i]);
            file = std::move(placed.file);
            voices.emplace_back(free[i], edit::NewGbaSampleVoice(placed.address));
        }
        const std::vector<uint32_t> programs(free.begin(), free.begin() + static_cast<std::ptrdiff_t>(samples.size()));
        DoGbaGroupVersion("Add Samples", gba_bank_view_->name + ", " + ProgramsText(programs), *bank_editor_,
                          edit::WithGbaVoicesAt(file, *played_.Gba(), *bank_editor_, voices));
        bank_state_.program = programs.front();
        bank_state_.gba_row.reset();
        bank_state_.gba_held.reset();
        bank_state_.gba_steps.reset();
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

bool App::DropGbaAssembly(const std::vector<std::string>& paths)
{
    // A file of assembly replaces the voice group.
    if (paths.size() == 1)
    {
        const std::filesystem::path file = xsflib::PathFromUtf8(paths.front());
        std::vector<uint8_t> bytes;
        if (xsflib::ReadFile(file, bytes) && !song::IsGbaSampleFile(bytes) && edit::IsGbaAssembly(bytes))
        {
            try
            {
                const std::string text(bytes.begin(), bytes.end());
                DoGbaGroupVersion("Replace Voice Group", gba_bank_view_->name, *bank_editor_,
                                  edit::WithGbaVoiceGroupFile(*Document(), *played_.Gba(), *bank_editor_, text));
            }
            catch (const std::exception& error)
            {
                ShowMessage("Can't add " + xsflib::Utf8(file.filename()) + " to the voice group. " +
                            format::AsSentence(error.what()));
            }
            return true;
        }
    }

    return false;
}

std::optional<std::vector<std::vector<uint8_t>>> App::ReadDroppedGbaSamples(const std::vector<std::string>& paths)
{
    std::vector<std::vector<uint8_t>> samples;
    for (const std::string& path : paths)
    {
        const std::filesystem::path file = xsflib::PathFromUtf8(path);
        const std::string name = xsflib::Utf8(file.filename());
        std::vector<uint8_t> bytes;
        if (!ReadChosenFile(file, bytes))
        {
            return std::nullopt;
        }

        try
        {
            samples.push_back(edit::ReadIncomingGbaSample(bytes).bytes);
        }
        catch (const std::exception& error)
        {
            ShowMessage("Can't add " + name + " to the voice group. " + format::AsSentence(error.what()));
            return std::nullopt;
        }
    }

    return samples;
}

void App::UpdateDriverBankEditor()
{
    const format::File* document = Document();
    if (!bank_editor_)
    {
        return;
    }
    if (document == nullptr || !archive_version_)
    {
        CloseBankEditor();
        return;
    }

    // The view follows each ROM that the program builds: the songs that play a sample can change.
    if (bank_view_version_ != archive_version_)
    {
        FollowDriverBankView();
    }
    TakeDriverSampleSongs();

    // An edit of a rate waits for the first read of the songs: the view then names the songs that the edit changes.
    bool open = true;
    const bool reading = driver_sample_songs_.state == DriverSongsState::kReading;
    const BankEdit bank_edit = DrawDriverBankEditor(*driver_bank_view_, bank_state_, ItemShown(bank_editor_),
                                                    EditorsWait() || reading, &open);
    ApplyDriverBankEdit(bank_edit);

    // The keyboard sounds its key on the selected sample. Another key, or none, lets the last key go.
    const auto make = [&](std::pair<uint32_t, int> sounded,
                          std::shared_ptr<std::atomic<bool>> release) -> std::unique_ptr<audio::Renderer>
    {
        return std::make_unique<song::DriverNotePreview>(
            played_.Driver(),
            played_.Driver()->SampleAudition(static_cast<int>(sounded.first), sounded.second, bank_state_.velocity),
            release, edit::ModelMixing(document->mixing));
    };
    SoundBankNote(open, make, "Can't play this sample. ");
    if (!open)
    {
        CloseBankEditor();
    }
}

void App::ApplyDriverBankEdit(const BankEdit& bank_edit)
{
    if (!bank_edit.refused.empty())
    {
        ShowMessage(bank_edit.refused);
        return;
    }
    if (!bank_edit.konami_sample)
    {
        return;
    }

    // The edit writes the sample table of the ROM that the current version builds. The view shows the new values at
    // once.
    try
    {
        FollowArchiveVersion();
        const song::KonamiRom* rom = song::KonamiOf(played_.Driver().get());
        if (rom == nullptr)
        {
            return;
        }
        format::File file = edit::WithKonamiSampleValues(*Document(), *rom, bank_edit.konami_sample->first,
                                                         bank_edit.konami_sample->second);
        Do(bank_edit.name, bank_edit.details, std::move(file));
        FollowArchiveVersion();
        FollowDriverBankView();
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::FollowDriverBankView()
{
    // The samples show at once. Reading the songs of a large ROM takes up to a second: a thread reads them, and the
    // view keeps the last ROM's songs until then. The ROM stays as it is. A ROM whose songs can't be read shows none.
    // Before the first read ends, the view says that it reads them.
    const std::shared_ptr<const song::DriverRom> rom = played_.Driver();
    driver_bank_view_ = MakeDriverBankView(*rom, driver_sample_songs_);
    bank_view_version_ = archive_version_;
    const auto read = [rom]
    {
        try
        {
            return SongsOfDriverSamples(*rom);
        }
        catch (const std::exception&)
        {
            return DriverSampleSongs{.state = DriverSongsState::kUnreadable};
        }
    };
    DropDriverSampleSongs();
    reading_sample_songs_ = std::async(std::launch::async, read);
}

void App::TakeDriverSampleSongs()
{
    if (!reading_sample_songs_.valid() || !Done(reading_sample_songs_))
    {
        return;
    }

    driver_sample_songs_ = reading_sample_songs_.get();
    if (driver_bank_view_)
    {
        ShowDriverSampleSongs(*driver_bank_view_, driver_sample_songs_);
    }
}

void App::DropDriverSampleSongs()
{
    if (reading_sample_songs_.valid() && !Done(reading_sample_songs_))
    {
        dropped_sample_songs_.push_back(std::move(reading_sample_songs_));
    }
    reading_sample_songs_ = {};
}

} // namespace hokuto::app
