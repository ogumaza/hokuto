// SPDX-License-Identifier: MIT

// The Bank Editor on each console: opening and closing it, following the file, its edits, and the files dropped on a
// bank. app_bank_editor_gba.cpp contains the GBA's editors, and app_bank_editor_ds.cpp those of the DS and DSE.

#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <utility>

#include "ntdlib/rsar/wave_file.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "edit/effects.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/preview.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

// The version of the Wii's wave files (RWAV) that a WAV file dropped on a bank of the Wii becomes.
constexpr uint16_t kWiiWaveVersion = 0x0102;

} // namespace

void App::OpenEditor(const BrowserPlace& place)
{
    const auto group = static_cast<std::size_t>(place.group);
    if (!ArchiveOpen() || place.group < 0 || place.item < 0 || group >= browser_.size() ||
        static_cast<std::size_t>(place.item) >= browser_[group].items.size() ||
        (played_.Driver() && group != kBanksGroup))
    {
        return;
    }

    // A bank opens in the Bank Editor, and an instrument opens there selected. A wave opens in its properties, and so
    // does a stream sound's stream. On the Wii, a wave archive is the wave data of a bank or of a wave sound file, by
    // its file's ID. On the GBA, a voice group opens in the Bank Editor by its address, and a voice by its program. On
    // the DS, a bank opens there by its index, and a program selected.
    const auto item = static_cast<std::size_t>(place.item);
    const uint32_t index = browser_[group].items[item].number;
    const bool listed =
        place.child >= 0 && static_cast<std::size_t>(place.child) < browser_[group].items[item].child_numbers.size();
    const std::optional<uint32_t> child =
        listed
            ? std::optional<uint32_t>(browser_[group].items[item].child_numbers[static_cast<std::size_t>(place.child)])
            : std::nullopt;
    if (group == kBanksGroup)
    {
        OpenBankEditor(index, child);
    }
    else if (group == kWavesGroup && child)
    {
        OpenWaveProperties(index, *child);
    }
    else if (group == kSoundsGroup && place.child < 0 && played_.Sar() && song::StreamFileOf(*played_.Sar(), index))
    {
        OpenStreamProperties(index);
    }
    else if (group == kSoundsGroup && place.child < 0 && played_.Wii() && song::StreamFileOf(*played_.Wii(), index))
    {
        OpenStreamProperties(index);
    }
    else if (group == kSoundsGroup && place.child < 0 && played_.Ds())
    {
        OpenStreamProperties(index);
    }
}

void App::OpenBankEditor(uint32_t bank, std::optional<uint32_t> program)
{
    if (bank_editor_ != bank)
    {
        CloseBankEditor();
        bank_editor_ = bank;
    }
    if (program)
    {
        bank_state_.program = program;
        bank_state_.region.reset();
        bank_state_.reveal = true;
    }
    ImGui::SetWindowFocus("###BankEditor");
}

void App::CloseBankEditor()
{
    if (bank_release_)
    {
        bank_release_->store(true, std::memory_order_release);
        bank_release_.reset();
    }
    bank_note_.reset();
    bank_editor_.reset();
    bank_view_.reset();
    gba_bank_view_.reset();
    ds_bank_view_.reset();
    dse_bank_view_.reset();
    driver_bank_view_.reset();
    bank_view_version_.reset();
    driver_sample_songs_ = {};
    DropDriverSampleSongs();
    bank_state_ = {};
    waiting_bank_drop_.reset();
    DropGroupSizes(bank_group_sizes_);
}

void App::UpdateBankEditor()
{
    OpenAddedBank();

    if (played_.Gba())
    {
        UpdateGbaBankEditor();
        return;
    }
    if (played_.Ds())
    {
        UpdateDsBankEditor();
        return;
    }
    if (played_.Dse())
    {
        UpdateDseBankEditor();
        return;
    }
    if (played_.Driver())
    {
        UpdateDriverBankEditor();
        return;
    }

    UpdateSarBankEditor();
}

void App::UpdateSarBankEditor()
{
    if (!bank_editor_)
    {
        return;
    }

    const std::size_t banks = played_.Wii()   ? played_.Wii()->Banks().size()
                              : played_.Sar() ? played_.Sar()->Banks().size()
                                              : 0;
    const format::File* document = Document();
    if (document == nullptr || !archive_version_ || *bank_editor_ >= banks)
    {
        CloseBankEditor();
        return;
    }

    // The view follows each archive that the program builds, and each move in the history back to the archive that the
    // program holds (FollowHistory).
    if (!FollowBankArchive())
    {
        return;
    }

    // Files that waited for a field's text go on the bank once the reader has stopped building and reading another
    // version (`reading_version_`). DropFilesOnBank refuses them with a message when the build failed or a window
    // waits.
    TakeWaitingBankDrop();

    // While the program builds the archive of another version, or another window waits, the editor takes no edits.
    const bool busy = EditorsWait();
    TakeGroupSizes(bank_group_sizes_, bank_view_->sizes);
    bool open = true;
    const BankEdit bank_edit = DrawBankEditor(*bank_view_, bank_state_, ItemShown(bank_editor_), busy, &open);

    // The keyboard sounds its key on the selected instrument, over the playback. Another key, or none, lets the last
    // key go.
    const auto make = [&](std::pair<uint32_t, int> sounded,
                          std::shared_ptr<std::atomic<bool>> release) -> std::unique_ptr<audio::Renderer>
    {
        // On the Wii, the note plays by the rules of the file's build of NW4R SND.
        if (played_.Wii())
        {
            std::optional<ntdlib::SndBuild> build;
            if (document != nullptr && document->native)
            {
                build = format::BuildOfWiiArchive(*document->native);
            }
            return std::make_unique<song::NotePreview>(played_.Wii(), std::vector<uint32_t>{*bank_editor_}, 0,
                                                       sounded.first, sounded.second, bank_state_.velocity, release,
                                                       build);
        }

        const std::vector<uint32_t> slots = {ntdlib::sar::ItemId(ntdlib::sar::kBankItem, *bank_editor_)};
        const format::Mixing mixing = FileMixing();
        return std::make_unique<song::NotePreview>(played_.Sar(), slots, 0, sounded.first, sounded.second,
                                                   bank_state_.velocity, release, edit::ModelMixing(mixing));
    };
    SoundBankNote(open, make, "Can't play this instrument. ");
    if (!open)
    {
        CloseBankEditor();
        return;
    }

    ApplyBankEdit(bank_edit);
}

void App::OpenAddedBank()
{
    // A bank that Project > Add Bank… added opens once the program has built the archive that holds it.
    const std::size_t banks = played_.Ds()    ? played_.Ds()->archive.banks.size()
                              : played_.Wii() ? played_.Wii()->Banks().size()
                              : played_.Sar() ? played_.Sar()->Banks().size()
                                              : 0;
    if (bank_to_open_ && !EditorsWait())
    {
        const uint32_t bank = *std::exchange(bank_to_open_, std::nullopt);
        if (bank < banks)
        {
            OpenBankEditor(bank);
        }
    }
}

bool App::FollowBankArchive()
{
    // The edits' refusals already say that Hokuto can't edit the bank.
    const auto make = [this]
    {
        bank_view_ = played_.Wii() ? MakeBankView(*archive_version_, *played_.Wii(), *bank_editor_)
                                   : MakeBankView(*archive_version_, *played_.Sar(), *bank_editor_);
        MeasureGroups(bank_group_sizes_, played_.Wii() ? played_.Wii()->Banks()[*bank_editor_].file_id
                                                       : played_.Sar()->Banks()[*bank_editor_].file_id);
    };
    return FollowBankView(make, "Can't edit this bank. ");
}

bool App::FollowBankView(const std::function<void()>& make, std::string_view refusal)
{
    if (bank_view_version_ != archive_version_)
    {
        try
        {
            make();
            bank_view_version_ = archive_version_;
        }
        catch (const std::exception& error)
        {
            const std::string message = error.what();
            ShowMessage(message.starts_with(refusal) ? message : std::string(refusal) + format::AsSentence(message));
            CloseBankEditor();
            return false;
        }
    }

    return true;
}

void App::TakeWaitingBankDrop()
{
    if (waiting_bank_drop_ && reading_version_ == nullptr)
    {
        const WaitingBankDrop waiting = *std::exchange(waiting_bank_drop_, std::nullopt);
        DropFilesOnBank(waiting.paths, waiting.drop);
    }
}

void App::SoundBankNote(bool sounds,
                        const std::function<std::unique_ptr<audio::Renderer>(std::pair<uint32_t, int>,
                                                                             std::shared_ptr<std::atomic<bool>>)>& make,
                        std::string_view refusal)
{
    std::optional<std::pair<uint32_t, int>> note;
    if (sounds && bank_state_.sounding >= 0 && bank_state_.program)
    {
        note = std::pair(*bank_state_.program, bank_state_.sounding);
    }
    if (note == bank_note_)
    {
        return;
    }

    if (bank_release_)
    {
        bank_release_->store(true, std::memory_order_release);
        bank_release_.reset();
    }
    bank_note_ = note;
    if (!note)
    {
        return;
    }

    auto release = std::make_shared<std::atomic<bool>>(false);
    try
    {
        PlayAudition(make(*note, release));
        bank_release_ = std::move(release);
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string(refusal) + format::AsSentence(error.what()));
    }
}

void App::ApplyBankEdit(const BankEdit& bank_edit)
{
    if (!bank_edit.refused.empty())
    {
        ShowMessage(bank_edit.refused);
        return;
    }
    if (bank_edit.wave)
    {
        DropWaveOnBank(*bank_edit.wave, bank_edit.drop);
        return;
    }
    if (!bank_edit.model)
    {
        return;
    }

    try
    {
        format::File file = played_.Wii()
                                ? edit::WithBankModel(*Document(), *played_.Wii(), *bank_editor_, *bank_edit.model)
                                : edit::WithBankModel(*Document(), *played_.Sar(), *bank_editor_, *bank_edit.model);
        DoBankEdit(bank_edit.name, bank_edit.details, std::move(file), *bank_edit.model);
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::DoBankEdit(std::string name, std::string details, format::File file, ntdlib::sar::BankFile model)
{
    const format::File* before = Document();
    Do(std::move(name), std::move(details), std::move(file));
    if (Document() == before || !bank_view_)
    {
        return;
    }

    // The editor shows the new model at once. The view follows the version's archive once the program has built it.
    bank_view_->model = std::move(model);
    bank_view_->entries.clear();
    for (uint32_t entry = 0; entry < bank_view_->model.waves.size(); ++entry)
    {
        bank_view_->entries.push_back(played_.Wii() ? edit::WaveEntryName(*played_.Wii(), bank_view_->model, entry)
                                                    : edit::WaveEntryName(*played_.Sar(), bank_view_->model, entry));
    }
}

void App::DoBankDrop(DroppedWaves dropped, format::File file)
{
    const format::File* before = Document();
    const std::string details = bank_view_->name + ", " + ProgramsText(dropped.programs);
    DoBankEdit(std::move(dropped.name), details, std::move(file), std::move(dropped.bank));
    if (Document() == before || dropped.programs.empty())
    {
        return;
    }

    bank_state_.program = dropped.programs.front();
    bank_state_.region = dropped.region;
    bank_state_.held.reset();
    bank_state_.reveal = true;
}

void App::DropFilesOnBank(const std::vector<std::string>& paths, const BankDrop& drop)
{
    const format::File* document = Document();
    if (!bank_editor_ || (!bank_view_ && !ds_bank_view_ && !gba_bank_view_) || document == nullptr || EditorsWait())
    {
        ShowMessage("Can't add files to the bank while an archive is building or another window is open.");
        return;
    }

    // A field that's being typed in keeps its text for the region or the program that the user typed it for. The files
    // then wait while the reader builds and reads another version, such as that of the field's edit (UpdateBankEditor).
    // They go nowhere when the editor closes or shows another bank meanwhile.
    const auto wait = [this, paths, drop, bank = *bank_editor_]
    {
        if (bank_editor_ == bank)
        {
            waiting_bank_drop_ = WaitingBankDrop{.paths = paths, .drop = drop};
        }
    };
    if (WaitForTyping(wait))
    {
        return;
    }

    if (played_.Wii())
    {
        DropFilesOnWiiBank(paths, drop);
        return;
    }
    if (played_.Ds())
    {
        DropFilesOnDsBank(paths, drop);
        return;
    }
    if (played_.Gba())
    {
        DropFilesOnGbaBank(paths, drop);
        return;
    }
    DropFilesOnSarBank(paths, drop, *document);
}

void App::DropFilesOnSarBank(const std::vector<std::string>& paths, const BankDrop& drop, const format::File& document)
{
    // Each file is a WAV file or a wave file. Its wave goes after the waves of the bank's wave archive, or into a new
    // one.
    const auto wave_archive = edit::WaveArchiveOfBank(bank_view_->model, *played_.Sar())
                                  .value_or(static_cast<uint32_t>(played_.Sar()->WaveArchives().size()));
    std::vector<edit::IncomingWave> incoming;
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
            incoming.push_back(edit::IsWaveFile(bytes)
                                   ? edit::ReadIncomingWaveFile(bytes, *played_.Sar(), wave_archive)
                                   : edit::ReadIncomingWave(bytes, *played_.Sar(), wave_archive, std::nullopt));
        }
        catch (const std::exception& error)
        {
            ShowMessage("Can't add " + name + " to the bank. " + format::AsSentence(error.what()));
            return;
        }
    }

    // The waves make regions or instruments, and the bank names them.
    try
    {
        const edit::BankWaves added =
            edit::WithBankWaves(document, *played_.Sar(), *bank_editor_, bank_view_->model, incoming);
        DroppedWaves dropped = WithDroppedWaves(bank_view_->model, drop, added.waves);
        format::File file = edit::WithBankModel(added.file, *played_.Sar(), *bank_editor_, dropped.bank);
        DoBankDrop(std::move(dropped), std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::DropFilesOnWiiBank(const std::vector<std::string>& paths, const BankDrop& drop)
{
    // On the Wii, each file is a WAV file or a wave file of the Wii (RWAV). Its wave goes after the bank's waves, in
    // the bank's wave data. A wave from a WAV file keeps its model.
    const format::File& document = *Document();
    std::vector<ntdlib::rsar::WaveFile> waves;
    std::vector<std::optional<format::WaveModel>> models;
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
            if (bytes.size() >= 4 && std::memcmp(bytes.data(), "RWAV", 4) == 0)
            {
                waves.push_back(ntdlib::rsar::WaveFile::Read(bytes));
                models.emplace_back();
            }
            else
            {
                const edit::IncomingWave incoming = edit::ReadIncomingWiiWave(bytes, kWiiWaveVersion);
                waves.push_back(ntdlib::rsar::WaveFile::Read(incoming.bytes));
                models.push_back(incoming.model);
            }
        }
        catch (const std::exception& error)
        {
            ShowMessage("Can't add " + name + " to the bank. " + format::AsSentence(error.what()));
            return;
        }
    }

    // The waves make regions or instruments, and the bank names them.
    try
    {
        const uint32_t file_id = played_.Wii()->Banks()[*bank_editor_].file_id;
        const std::vector<ntdlib::sar::WaveId> ids = edit::NewWaveIds(*played_.Wii(), *bank_editor_, waves.size());
        DroppedWaves dropped = WithDroppedWaves(bank_view_->model, drop, ids);
        format::File file =
            edit::WithBankModel(document, *played_.Wii(), *bank_editor_, dropped.bank, std::move(waves));
        for (std::size_t k = 0; k < ids.size(); ++k)
        {
            if (models[k])
            {
                file.waves[{file_id, ids[k].index}] = *models[k];
            }
        }
        DoBankDrop(std::move(dropped), std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::DropWaveOnBank(const BrowserPlace& place, const BankDrop& drop)
{
    const std::optional<ListedPlace> entry = ListedAt(place);
    if (!bank_editor_ || !bank_view_ || place.group != static_cast<int>(kWavesGroup) || !entry || !entry->child)
    {
        return;
    }

    // The wave makes a region or an instrument, and the bank names it. On the Wii, the wave archive is a file's wave
    // data, by the file's ID, and a wave of another file comes into the bank as a copy.
    const ntdlib::sar::WaveId wave{ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, entry->number), *entry->child};
    try
    {
        DroppedWaves dropped = WithDroppedWaves(bank_view_->model, drop, {wave});
        format::File file = played_.Wii()
                                ? edit::WithBankModel(*Document(), *played_.Wii(), *bank_editor_, dropped.bank)
                                : edit::WithBankModel(*Document(), *played_.Sar(), *bank_editor_, dropped.bank);
        DoBankDrop(std::move(dropped), std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

} // namespace hokuto::app
