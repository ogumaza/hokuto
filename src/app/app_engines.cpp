// SPDX-License-Identifier: MIT

// The engine and the mixing mode that the shown sound plays with, its conversion for another console's engine, and the
// model that plays it.

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <utility>

#include "app/app.h"
#include "app/app_internal.h"
#include "app/engine_names.h"
#include "edit/bytecode.h"
#include "edit/effects.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/driver.h"

namespace hokuto::app
{

void App::ApplyModeRequest()
{
    const std::optional<bool> timeline = song_ ? std::exchange(song_->mode_request, std::nullopt) : std::nullopt;
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!timeline || document == nullptr || !archive_version_ || !sequence ||
        !SameArchive(*document, *archive_version_) || *timeline == edit::IsTimeline(*document, *sequence))
    {
        return;
    }

    // The sequence's commands take the other layout, and the editors' selections go.
    try
    {
        format::File file = *timeline ? edit::AsTimeline(*document, *sequence) : edit::AsProgram(*document, *sequence);
        Do(*timeline ? "Edit as Timeline" : "Edit as Program", "Sound: " + song_->name, std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyMixingRequest()
{
    const std::optional<bool> enhanced = song_ ? std::exchange(song_->mixing_request, std::nullopt) : std::nullopt;
    const format::File* document = Document();
    if (!enhanced || document == nullptr || !sequence_)
    {
        return;
    }

    const format::Mixing mixing = *enhanced ? format::Mixing::kEnhanced : format::Mixing::kStrict;
    if (format::MixingOf(*document, sequence_->sound) != mixing)
    {
        Do(*enhanced ? "Enhanced Mixing" : "Strict Mixing", "Sound: " + song_->name,
           format::WithMixing(*document, sequence_->sound, mixing));
    }
}

song::Mixing App::ShownMixing() const
{
    const format::File* document = Document();
    if (document == nullptr || !sequence_)
    {
        return song::Mixing::kStrict;
    }

    return PlayedMixing(*document, sequence_->sound);
}

void App::ApplyEngineRequest()
{
    const std::optional<int> requested = song_ ? std::exchange(song_->engine_request, std::nullopt) : std::nullopt;
    const format::File* document = Document();
    if (!requested || document == nullptr || !sequence_ || *requested < 0 ||
        *requested >= static_cast<int>(format::kEngineCount))
    {
        return;
    }

    const auto engine = static_cast<format::Engine>(*requested);
    if (!edit::EngineRefused(*document, engine) && format::EngineOf(*document, sequence_->sound) != engine)
    {
        const uint32_t sound = sequence_->sound;
        const std::size_t before = history_->Done();
        Do("Change Engine", "Sound: " + song_->name + " - " + EngineName(engine),
           format::WithEngine(*document, sound, engine));
        if (history_->Done() == before + 1)
        {
            engine_edit_ = EngineEdit{history_->Done(), sound, engine};
        }
    }
}

format::Engine App::ShownEngine() const
{
    const format::File* document = Document();
    if (document == nullptr)
    {
        return format::Engine::kSwitch;
    }
    if (!sequence_)
    {
        return document->engine;
    }

    const format::Engine engine = format::EngineOf(*document, sequence_->sound);
    return edit::EngineRefused(*document, engine) ? document->engine : engine;
}

std::size_t App::ShownSends() const
{
    const format::File* document = Document();
    if (document == nullptr || !document->native)
    {
        return kSendCount;
    }

    // Count down from the last send to the first one that both kinds of track run.
    const std::optional<ntdlib::sar::Flavor> flavor = format::FlavorOf(ShownEngine());
    std::size_t count = kSendCount;
    while (count > 0)
    {
        const auto bus = static_cast<uint8_t>(count - 1);
        if (song::Runs(edit::ConsoleOf(*document), ntdlib::commands::Kind::kSend, bus) &&
            (!flavor || song::Runs(*flavor, ntdlib::commands::Kind::kSend, bus)))
        {
            break;
        }

        count--;
    }

    return count;
}

format::Engine App::PlayedEngine() const
{
    if (engine_sound_)
    {
        return engine_sound_->played.Engine();
    }

    const format::File* document = Document();
    return document != nullptr ? document->engine : format::Engine::kSwitch;
}

std::shared_ptr<const ntdlib::sar::SoundArchive> App::PlayedArchive() const
{
    return engine_sound_ ? engine_sound_->played.Sar() : played_.Sar();
}

std::shared_ptr<const ntdlib::rsar::SoundArchive> App::PlayedWiiArchive() const
{
    return engine_sound_ ? engine_sound_->played.Wii() : played_.Wii();
}

std::shared_ptr<const song::DsArchive> App::PlayedDsArchive() const
{
    return engine_sound_ ? engine_sound_->played.Ds() : played_.Ds();
}

const song::Sequence& App::PlayedSequence() const
{
    return engine_sound_ && engine_sequence_ ? *engine_sequence_ : *sequence_;
}

song::SequenceReader::Convert App::ConvertFor(const std::shared_ptr<const format::File>& version, uint32_t sound)
{
    converting_engine_.reset();
    converting_version_.reset();
    const format::Engine engine = format::EngineOf(*version, sound);
    if (engine == version->engine || edit::EngineRefused(*version, engine))
    {
        return {};
    }

    converting_engine_ = engine;
    converting_version_ = version;

    // The last conversion of the shown sound serves a version that builds the same archive. So does its error.
    const bool same =
        sequence_ && sequence_->sound == sound && engine_version_ && SameArchive(*version, *engine_version_);
    if (same && engine_sound_ && PlayedEngine() == engine)
    {
        return [converted = *engine_sound_]
        {
            return converted;
        };
    }
    if (same && !engine_sound_ && engine_failed_ == engine)
    {
        return [error = engine_error_]() -> song::SequenceReader::EngineSound
        {
            throw std::invalid_argument(error);
        };
    }

    // The conversion's file holds the sound alone. A conversion for the Wii plays by the rules of its file's build of
    // NW4R SND.
    return [version, sound, engine, name = file_name_]
    {
        edit::OtherConsoleSong converted = edit::ForEngine(*version, sound, engine);
        if (engine == format::Engine::kDs)
        {
            song::SequenceReader::EngineSound ds{.warnings = std::move(converted.warnings)};
            ds.played = song::LoadDsArchive(format::ExportArchive(converted.file));
            ds.slot_programs = std::move(converted.slot_programs);
            return ds;
        }
        if (engine == format::Engine::kWii)
        {
            song::SequenceReader::EngineSound wii{.warnings = std::move(converted.warnings)};
            wii.played = WiiArchiveOf(converted.file, name);
            wii.wii_build = format::BuildOfWiiArchive(*converted.file.native);
            wii.slot_programs = std::move(converted.slot_programs);
            return wii;
        }

        return song::SequenceReader::EngineSound{
            .played = song::LoadArchive(format::ExportArchive(converted.file), name),
            .sound = 0,
            .warnings = std::move(converted.warnings)};
    };
}

bool App::EngineServes(const format::File& version, uint32_t sound) const
{
    const format::Engine engine = format::EngineOf(version, sound);
    if (engine == version.engine || edit::EngineRefused(version, engine))
    {
        return !engine_sound_;
    }

    const bool same =
        sequence_ && sequence_->sound == sound && engine_version_ && SameArchive(version, *engine_version_);
    return same && (engine_sound_ ? PlayedEngine() == engine : engine_failed_ == engine);
}

void App::TakeEngine(song::SequenceReader::Result& read)
{
    // The latest request converted for its engine from its version.
    const std::optional<format::Engine> engine = std::exchange(converting_engine_, std::nullopt);
    const std::shared_ptr<const format::File> version = std::exchange(converting_version_, nullptr);
    const bool known = engine && engine_failed_ == engine && engine_error_ == read.engine_error;
    engine_sound_ = std::move(read.engine);
    engine_sequence_ = std::move(read.engine_sequence);
    engine_tracks_ = engine_sequence_ && song::OnGbaOrDs(played_)
                         ? std::optional(song::TracksOfConversion(*read.sequence, *engine_sequence_))
                         : std::nullopt;
    engine_version_ = engine ? version : nullptr;
    engine_error_ = std::move(read.engine_error);
    engine_failed_ = engine_error_.empty() ? std::nullopt : engine;
    const bool dropped = engine && DropEngineEdit(*engine, read.sequence->sound, !engine_error_.empty());
    const format::File* document = Document();
    if (!engine_error_.empty() && engine && (!known || dropped) && document != nullptr)
    {
        ShowMessage("Can't play the sound on " + TheEngine(*engine) + ". " + format::AsSentence(engine_error_) +
                    " The sound " + (dropped ? "stays" : "plays") + " on " + TheEngine(document->engine) + ".");
    }
}

bool App::DropEngineEdit(format::Engine engine, uint32_t sound, bool failed)
{
    // The first conversion for the engine settles the action. A later action keeps it.
    const std::optional<EngineEdit> edit = std::exchange(engine_edit_, std::nullopt);
    if (!failed || !edit || !history_ || edit->engine != engine || edit->sound != sound ||
        history_->Done() != edit->done || history_->Actions().size() != edit->done)
    {
        return false;
    }

    // The sound keeps the engine that it had, and the Undo History loses the row.
    const std::shared_ptr<const format::File> left = history_->Current();
    history_->Undo();
    history_->Forget();
    history_rows_.resize(history_->Done());
    action_selections_.resize(history_->Done());
    FollowHistory(edit->done, *left);

    return true;
}

void App::ForgetEngine()
{
    engine_sound_.reset();
    engine_sequence_.reset();
    engine_tracks_.reset();
    engine_version_.reset();
    engine_error_.clear();
    engine_failed_.reset();
    converting_engine_.reset();
    converting_version_.reset();
}

std::shared_ptr<song::SequenceModel> App::MakeModel() const
{
    // On another console's engine, the model plays the sound's conversion. The model plays only the effects of its
    // console. The DS has none.
    const song::ModelOptions options{.effects = edit::ModelEffects(model_effects_), .mixing = model_mixing_};
    if (engine_sound_ && engine_sequence_)
    {
        return std::make_shared<song::SequenceModel>(engine_sound_->played, engine_sequence_, options);
    }

    return std::make_shared<song::SequenceModel>(played_, sequence_, options);
}

} // namespace hokuto::app
