// SPDX-License-Identifier: MIT

// The Bank Editor on the DS: the banks of the DS's engine and of DSE, their edits, and the waves dropped on a DS bank.

#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "edit/ds_waves.h"
#include "edit/dse_banks.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/preview.h"

namespace hokuto::app
{

void App::UpdateDseBankEditor()
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

    // The view follows each set that the program builds, and each move in the history back to the set that the program
    // has (FollowHistory). A version whose set doesn't contain the bank closes the editor.
    if (!FollowBankView([this] { dse_bank_view_ = MakeDseBankView(*played_.Dse(), *bank_editor_); },
                        "Can't edit this bank. "))
    {
        return;
    }

    const bool busy = EditorsWait();
    bool open = true;
    const BankEdit bank_edit = DrawDseBankEditor(*dse_bank_view_, bank_state_, ItemShown(bank_editor_), busy, &open);

    // Like the Key Editor's Acoustic Feedback, the keyboard sounds its key on the selected program. Another key, or
    // none, lets the last key go.
    const auto make = [&](std::pair<uint32_t, int> sounded,
                          std::shared_ptr<std::atomic<bool>> release) -> std::unique_ptr<audio::Renderer>
    {
        return std::make_unique<song::DseNotePreview>(
            played_.Dse()->banks.at(*bank_editor_), static_cast<int>(sounded.first), sounded.second,
            bank_state_.velocity, release, played_.Dse()->build, edit::ModelMixing(document->mixing));
    };
    SoundBankNote(open, make, "Can't play this instrument. ");
    if (!open)
    {
        CloseBankEditor();
        return;
    }

    ApplyDseBankEdit(bank_edit);
}

void App::ApplyDseBankEdit(const BankEdit& bank_edit)
{
    if (!bank_edit.refused.empty())
    {
        ShowMessage(bank_edit.refused);
        return;
    }
    if (!bank_edit.dse_split)
    {
        return;
    }

    try
    {
        FollowArchiveVersion();
        format::File file =
            edit::WithDseSplit(*Document(), *played_.Dse(), bank_edit.dse_split->first, bank_edit.dse_split->second);
        const format::File* before = Document();
        Do(bank_edit.name, bank_edit.details, std::move(file));
        if (Document() == before || !dse_bank_view_)
        {
            return;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return;
    }

    // The editor shows the new values at once. The view follows the version's set once the program has built it.
    const auto& [place, split] = *bank_edit.dse_split;
    for (DseProgramView& program : dse_bank_view_->programs)
    {
        if (program.program == place.program && place.split < program.rows.size())
        {
            program.rows[place.split].values = split;
            program.rows[place.split].wave = "Wave " + std::to_string(edit::DseSplitWave(split));
        }
    }
}

void App::UpdateDsBankEditor()
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

    // The view follows each archive that the program builds, and each move in the history back to the archive that the
    // program has (FollowHistory). A version whose archive doesn't contain the bank closes the editor.
    if (!FollowBankView([this] { ds_bank_view_ = MakeDsBankView(*played_.Ds(), *bank_editor_); },
                        "Can't edit this bank. "))
    {
        return;
    }

    // As on the later consoles, files that waited for a field's text go on the bank (UpdateBankEditor).
    TakeWaitingBankDrop();

    const bool busy = EditorsWait();
    bool open = true;
    const BankEdit bank_edit = DrawDsBankEditor(*ds_bank_view_, bank_state_, ItemShown(bank_editor_), busy, &open);

    // Like the Key Editor's Acoustic Feedback, the keyboard sounds its key on the selected program. Another key, or
    // none, lets the last key go.
    const auto make = [&](std::pair<uint32_t, int> sounded,
                          std::shared_ptr<std::atomic<bool>> release) -> std::unique_ptr<audio::Renderer>
    {
        return std::make_unique<song::DsNotePreview>(*played_.Ds(), *bank_editor_, static_cast<int>(sounded.first),
                                                     sounded.second, bank_state_.velocity, release,
                                                     edit::ModelMixing(document->mixing));
    };
    SoundBankNote(open, make, "Can't play this instrument. ");
    if (!open)
    {
        CloseBankEditor();
        return;
    }

    ApplyDsBankEdit(bank_edit);
}

void App::ApplyDsBankEdit(const BankEdit& bank_edit)
{
    if (!bank_edit.refused.empty())
    {
        ShowMessage(bank_edit.refused);
        return;
    }
    if (bank_edit.wave)
    {
        DropWaveOnDsBank(*bank_edit.wave, bank_edit.drop);
        return;
    }

    // A new model or new slots build the archive again, and the view shows them at once.
    if (bank_edit.ds_model || bank_edit.ds_slots)
    {
        try
        {
            FollowArchiveVersion();
            format::File file =
                bank_edit.ds_model
                    ? edit::WithDsBankModel(*Document(), *played_.Ds(), *bank_editor_, *bank_edit.ds_model)
                    : edit::WithDsBankSlots(*Document(), *bank_editor_, *bank_edit.ds_slots);
            Do(bank_edit.name, bank_edit.details, std::move(file));
            FollowArchiveVersion();
            ds_bank_view_ = MakeDsBankView(*played_.Ds(), *bank_editor_);
            bank_view_version_ = archive_version_;
        }
        catch (const std::exception& error)
        {
            ShowMessage(error.what());
        }
        return;
    }
    if (!bank_edit.ds_region)
    {
        return;
    }

    try
    {
        FollowArchiveVersion();
        format::File file =
            edit::WithDsRegion(*Document(), *played_.Ds(), bank_edit.ds_region->first, bank_edit.ds_region->second);
        const format::File* before = Document();
        Do(bank_edit.name, bank_edit.details, std::move(file));
        if (Document() == before || !ds_bank_view_)
        {
            return;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return;
    }

    // The editor shows the new values of the edited program at once. The view follows the version's archive once the
    // program has built it.
    const auto& [place, region] = *bank_edit.ds_region;
    for (DsProgram& program : ds_bank_view_->programs)
    {
        for (DsRegionRow& row : program.rows)
        {
            if (program.program == place.program && row.region == place.region)
            {
                row.values = region;
                row.kind = DsKindName(region.type);
            }
        }
    }
}

void App::DropFilesOnDsBank(const std::vector<std::string>& paths, const BankDrop& drop)
{
    std::vector<std::vector<uint8_t>> waves;
    for (const std::string& path : paths)
    {
        const std::filesystem::path file = xsflib::PathFromUtf8(path);
        const std::string name = xsflib::Utf8(file.filename());
        std::vector<uint8_t> bytes;
        if (!ReadChosenFile(file, bytes))
        {
            return;
        }

        try
        {
            waves.push_back(edit::IsDsWaveFile(bytes) ? edit::ReadIncomingDsWaveFile(bytes).bytes
                                                      : edit::ReadIncomingDsWave(bytes).bytes);
        }
        catch (const std::exception& error)
        {
            ShowMessage("Can't add " + name + " to the bank. " + format::AsSentence(error.what()));
            return;
        }
    }

    try
    {
        FollowArchiveVersion();
        const edit::DsNewWaves added = edit::DsNewWavePlaces(*played_.Ds(), *bank_editor_, waves.size());
        edit::DroppedDsWaves dropped =
            edit::WithDroppedDsWaves(edit::DsBankModelOf(*played_.Ds(), *bank_editor_),
                                     DroppedDsRegion(*ds_bank_view_, bank_state_, drop), added.places);
        format::File file =
            edit::WithDsBankModel(edit::WithDsNewWaves(*Document(), *played_.Ds(), added.wave_archive, waves),
                                  *played_.Ds(), *bank_editor_, dropped.bank);
        DoDsBankDrop(std::move(dropped), std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::DropWaveOnDsBank(const BrowserPlace& place, const BankDrop& drop)
{
    const std::optional<ListedPlace> entry = ListedAt(place);
    if (!bank_editor_ || !ds_bank_view_ || place.group != static_cast<int>(kWavesGroup) || !entry || !entry->child)
    {
        return;
    }

    try
    {
        FollowArchiveVersion();
        const std::optional<uint16_t> slot = edit::DsSlotOfWaveArchive(*played_.Ds(), *bank_editor_, entry->number);
        if (!slot)
        {
            throw std::invalid_argument("A region can use only waves from the wave archives of the bank. Add " +
                                        song::DsWaveArchiveName(*played_.Ds(), entry->number) +
                                        " under Wave Archives… first.");
        }

        const edit::DsWavePlace at{*slot, static_cast<uint16_t>(*entry->child)};
        edit::DroppedDsWaves dropped =
            edit::WithDroppedDsWaves(edit::DsBankModelOf(*played_.Ds(), *bank_editor_),
                                     DroppedDsRegion(*ds_bank_view_, bank_state_, drop), std::span(&at, 1));
        format::File file = edit::WithDsBankModel(*Document(), *played_.Ds(), *bank_editor_, dropped.bank);
        DoDsBankDrop(std::move(dropped), std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::DoDsBankDrop(edit::DroppedDsWaves dropped, format::File file)
{
    const format::File* before = Document();
    Do(std::move(dropped.name), ds_bank_view_->name + ", " + ProgramsText(dropped.programs), std::move(file));
    if (Document() == before)
    {
        return;
    }

    FollowArchiveVersion();
    ds_bank_view_ = MakeDsBankView(*played_.Ds(), *bank_editor_);
    bank_view_version_ = archive_version_;
    if (bank_state_.program != dropped.programs.front())
    {
        bank_state_.program = dropped.programs.front();
        bank_state_.ds_row.reset();
    }
    bank_state_.ds_held.reset();
    bank_state_.reveal = true;
}

} // namespace hokuto::app
