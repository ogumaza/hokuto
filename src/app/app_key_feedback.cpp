// SPDX-License-Identifier: MIT

// The notes that the Key Editor sounds: the audition of a clicked or drawn note on the console of the shown sound, or
// on the engine that plays it.

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "app/app.h"
#include "app/app_internal.h"
#include "song/driver.h"
#include "song/preview.h"

namespace hokuto::app
{

bool App::PlayEngineDsFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    try
    {
        const std::optional<uint32_t> bank = song::DsBankOf(*engine_sound_->played.Ds(), engine_sound_->sound);
        if (!bank)
        {
            return false;
        }
        const auto range = engine_sound_->slot_programs.find(note.bank_slot);
        const uint32_t program =
            note.program + (range != engine_sound_->slot_programs.end() ? range->second : uint32_t{0});
        PlayAudition(std::make_unique<song::DsNotePreview>(*engine_sound_->played.Ds(), *bank,
                                                           static_cast<int>(program), note.key, note.velocity, release,
                                                           model_mixing_));
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

bool App::PlayDriverFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    const std::optional<uint32_t> shown = ShownSequence();
    if (!shown || !archive_version_ || *shown >= archive_version_->sequences.size())
    {
        return false;
    }
    try
    {
        const std::optional<song::DriverAudition> audition =
            played_.Driver()->NoteAudition(*archive_version_->sequences[*shown], *sequence_, key_editor_track_,
                                           static_cast<int>(note.program), note.key, note.velocity);
        if (!audition)
        {
            return false;
        }
        PlayAudition(std::make_unique<song::DriverNotePreview>(played_.Driver(), *audition, release, model_mixing_));
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

bool App::PlayFeedbackPreview(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    if (engine_sound_ && engine_sound_->played.Ds())
    {
        // On the DS's engine, the note plays the conversion's one bank. A slot whose bank went into that bank plays the
        // programs of its range.
        if (!PlayEngineDsFeedback(note, release))
        {
            return false;
        }
    }
    else if (engine_sound_ && engine_sound_->played.Wii() &&
             engine_sound_->sound < engine_sound_->played.Wii()->Sounds().size())
    {
        // On the Wii's engine, the note plays the conversion's one bank. The notes of every slot play that bank there.
        // A slot whose bank went into that bank plays the programs of its range.
        PlayEngineWiiFeedback(note, release);
    }
    else if (engine_sound_ && engine_sound_->played.Sar() &&
             engine_sound_->sound < engine_sound_->played.Sar()->Sounds().size())
    {
        // On another console's engine, the note plays with the banks of the sound's conversion.
        const std::shared_ptr<const ntdlib::sar::SoundArchive>& archive = engine_sound_->played.Sar();
        PlayAudition(std::make_unique<song::NotePreview>(archive, archive->Sounds()[engine_sound_->sound].banks,
                                                         note.bank_slot, note.program, note.key, note.velocity, release,
                                                         model_mixing_));
    }
    else if (played_.Wii() && sequence_->sound < played_.Wii()->Sounds().size())
    {
        PlayAudition(std::make_unique<song::NotePreview>(
            played_.Wii(), std::vector<uint32_t>{played_.Wii()->Sounds()[sequence_->sound].bank}, note.bank_slot,
            note.program, note.key, note.velocity, release, sequence_->wii_build));
    }
    else if (played_.Sar() && sequence_->sound < played_.Sar()->Sounds().size())
    {
        PlayAudition(std::make_unique<song::NotePreview>(played_.Sar(), played_.Sar()->Sounds()[sequence_->sound].banks,
                                                         note.bank_slot, note.program, note.key, note.velocity, release,
                                                         model_mixing_));
    }
    else if (played_.Gba())
    {
        // On the GBA, the note plays the song's voice group, on the song's music player.
        if (!PlayGbaFeedback(note, release))
        {
            return false;
        }
    }
    else if (played_.Ds())
    {
        // On the DS, the note plays the sound's one bank.
        if (!PlayDsFeedback(note, release))
        {
            return false;
        }
    }
    else if (played_.Driver())
    {
        // On the GBA's other drivers, the note plays on its track, with the commands of the track's nearest note of its
        // program.
        if (!PlayDriverFeedback(note, release))
        {
            return false;
        }
    }
    else if (played_.Dse())
    {
        // On DSE, the note plays the bank of its slot among the sound's banks.
        if (!PlayDseFeedback(note, release))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    return true;
}

void App::PlayEngineWiiFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive = engine_sound_->played.Wii();
    const auto range = engine_sound_->slot_programs.find(note.bank_slot);
    const uint32_t program = note.program + (range != engine_sound_->slot_programs.end() ? range->second : uint32_t{0});
    PlayAudition(std::make_unique<song::NotePreview>(
        archive, std::vector<uint32_t>{archive->Sounds()[engine_sound_->sound].bank}, 0, program, note.key,
        note.velocity, release, engine_sound_->wii_build));
}

bool App::PlayGbaFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    try
    {
        PlayAudition(std::make_unique<song::GbaNotePreview>(played_.Gba(), static_cast<int>(sequence_->sound),
                                                            static_cast<int>(note.program), note.key, note.velocity,
                                                            release, model_mixing_));
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

bool App::PlayDsFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    try
    {
        const std::optional<uint32_t> bank = song::DsBankOf(*played_.Ds(), sequence_->sound);
        if (!bank)
        {
            return false;
        }
        PlayAudition(std::make_unique<song::DsNotePreview>(*played_.Ds(), *bank, static_cast<int>(note.program),
                                                           note.key, note.velocity, release, model_mixing_));
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

bool App::PlayDseFeedback(const KeyNote& note, const std::shared_ptr<std::atomic<bool>>& release)
{
    try
    {
        const ntdlib::sequence_export::dse::SequenceInput& sound = played_.Dse()->sounds.at(sequence_->sound);
        const song::DseBank bank{.bank = sound.banks.at(note.bank_slot), .main_bank = sound.main_bank};
        PlayAudition(std::make_unique<song::DseNotePreview>(bank, static_cast<int>(note.program), note.key,
                                                            note.velocity, release, played_.Dse()->build,
                                                            model_mixing_));
    }
    catch (const std::exception&)
    {
        return false;
    }

    return true;
}

void App::PlayFeedback(const std::optional<KeyNote>& note)
{
    const auto same = [&note](const KeyNote& playing)
    {
        return note && note->key == playing.key && note->velocity == playing.velocity &&
               note->bank_slot == playing.bank_slot && note->program == playing.program;
    };
    if (feedback_note_ && same(*feedback_note_))
    {
        return;
    }

    // The last note's release plays out, unless the next note takes its place.
    if (feedback_release_)
    {
        feedback_release_->store(true, std::memory_order_release);
        feedback_release_.reset();
    }
    feedback_note_.reset();

    // The next note plays with the sound's banks, over the playback. A Wii sound has one bank, and its notes play by
    // the rules of the sequence's build of NW4R SND.
    if (!note || !sequence_)
    {
        return;
    }

    auto release = std::make_shared<std::atomic<bool>>(false);
    if (!PlayFeedbackPreview(*note, release))
    {
        return;
    }

    feedback_note_ = note;
    feedback_release_ = std::move(release);
}

} // namespace hokuto::app
