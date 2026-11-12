// SPDX-License-Identifier: MIT

// The measure of an export: the renders of the export and of Hokuto's playback, and the levels that they compare.

#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rvl/snd/rsar_options.h"

#include "app/app.h"
#include "app/app_internal.h"
#include "app/engine_names.h"
#include "app/export_warnings.h"
#include "edit/commands.h"
#include "format/drivers.h"
#include "format/engines.h"
#include "format/file.h"
#include "format/kinds.h"
#include "measure/measure.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/dry_render.h"
#include "song/played.h"

namespace hokuto::app
{

namespace
{

// What neither render of the Export window's measure includes (MeasuredLines) for what `played` contains: the effects
// on the aux buses, the reverb of MusicPlayer2000's mixer on the GBA, or the echo of another GBA driver. The DS has
// none of them.
std::string Unplayed(const song::Played& played)
{
    switch (played.Family())
    {
    case format::Family::kGbaDriver:
        {
            const song::DriverRules& rules = played.Driver()->Rules();
            return rules.MixesEcho() ? "the echo of " + std::string(rules.Title()) : "";
        }

    case format::Family::kMp2k:
        return "the MusicPlayer2000 reverb";

    case format::Family::kNitroComposer:
    case format::Family::kDse:
        return "";

    case format::Family::kNintendoWare:
    case format::Family::kNw4r:
        break;
    }

    return "the effects on the aux buses";
}

// What a measure plays: the archive, the ROM or the set that plays the sequence, the sequence, and its mixing mode.
struct PlayedSource
{
    song::Played played;
    std::shared_ptr<const song::Sequence> sequence;
    song::Mixing mixing{};
};

// The dry render of the one sound of `target`, the new file of console `to`, for a measure of an export in `mixing`.
// Messages call the file `name`.
song::DrySource TargetDry(std::vector<uint8_t> target, const std::string& name, format::Engine to, song::Mixing mixing)
{
    if (to == format::Engine::kDs)
    {
        const std::shared_ptr<const song::DsArchive> archive = song::LoadDsArchive(std::move(target));
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
        return song::DryOf(archive, std::move(sequence), mixing);
    }
    if (to == format::Engine::kWii)
    {
        const std::shared_ptr<const ntdlib::rsar::SoundArchive> archive =
            song::LoadWiiArchive(std::move(target), name, ntdlib::rvl::snd::kRsarOptions);
        auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
        return song::DryOf(archive, std::move(sequence));
    }

    const std::shared_ptr<const ntdlib::sar::SoundArchive> archive = song::LoadArchive(std::move(target), name);
    auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, 0));
    // The new file plays in strict mixing where its console takes no other (format::MixingRefused).
    const song::Mixing target_mixing =
        format::MixingRefused(to, format::Mixing::kEnhanced) ? song::Mixing::kStrict : mixing;
    return song::DryOf(archive, std::move(sequence), target_mixing);
}

// Measures the export of `source` to the file `target` of console `to`, rendered on `model`. `random` marks a sound
// that draws random numbers.
MeasuredView MeasureOtherConsole(std::vector<uint8_t> target, const PlayedSource& source, bool random,
                                 format::Engine to, const std::string& model,
                                 const std::shared_ptr<std::atomic<bool>>& cancel)
{
    const auto& [played, sequence, mixing] = source;
    MeasuredView view;
    if (!played || !sequence)
    {
        view.status = "Couldn't measure the export because no sequence plays.";
        return view;
    }

    try
    {
        // The new file holds one sound. A file of the Wii plays by the rules of the build that its archive's
        // version gives. The file that edit::ForWii makes plays by the same rules.
        const song::DrySource target_dry = TargetDry(std::move(target), sequence->name, to, mixing);
        const measure::Renders renders =
            measure::RenderOtherConsole(song::DryOf(played, sequence, mixing), target_dry, *cancel);
        if (cancel->load())
        {
            return view;
        }

        const measure::Measure found = measure::MeasureRenders(renders);
        view.lines = MeasuredLines(found.comparison, model, Unplayed(played), "", found.seconds, found.cut, random);
        for (const measure::PlaceClips& place : found.places)
        {
            view.places.push_back({PlaceLine(place.place), place.export_clip, place.hokuto_clip, found.rate});
        }
    }
    catch (const std::exception& error)
    {
        view.status = std::string("Couldn't measure the export. ") + format::AsSentence(error.what());
    }

    return view;
}

// Measures the MIDI export `conversion` of `source` with `instruments`. `random` marks a sound that draws random
// numbers.
MeasuredView MeasureMidi(const ntdlib::sequence_export::Conversion& conversion, measure::Instruments instruments,
                         const PlayedSource& source, bool random, const std::shared_ptr<std::atomic<bool>>& cancel)
{
    const auto& [played, sequence, mixing] = source;
    MeasuredView view;
    if (!played || !sequence)
    {
        view.status = "Couldn't measure the export because no sequence plays.";
        return view;
    }

    try
    {
        // A song of the GBA is exported only as MIDI with SoundFont 2, and its measure renders it that way.
        const measure::Instruments rendered =
            played.Family() == format::Family::kMp2k ? measure::Instruments::kSoundFont : instruments;
        const measure::Measure found =
            measure::MeasureExport(conversion, rendered, song::DryOf(played, sequence, mixing), *cancel);
        // The MIDI file can't store the effects on the aux buses, the reverb of MusicPlayer2000's mixer or the echo
        // of another GBA driver.
        const bool sound_font = instruments == measure::Instruments::kSoundFont;
        std::string notes;
        if (sound_font)
        {
            notes =
                "The renderer follows the SoundFont 2.04 specification, except where common players depart from "
                "it. There it follows the players, for example by applying 0.4 of the attenuation of each zone.";
        }
        view.lines = MeasuredLines(found.comparison, sound_font ? "Hokuto's SoundFont 2 renderer" : "sfizz 1.2.3",
                                   Unplayed(played), notes, found.seconds, found.cut, random);
        view.closing = "Other players may play the files differently.";
        for (const measure::PlaceClips& place : found.places)
        {
            view.places.push_back({PlaceLine(place.place), place.export_clip, place.hokuto_clip, found.rate});
        }
    }
    catch (const std::exception& error)
    {
        view.status = std::string("Couldn't measure the export. ") + format::AsSentence(error.what());
    }

    return view;
}

} // namespace

void App::StartMeasure(Export& job)
{
    // The measure renders the sequence as the zones play it: the archive and the sequence that they play, in the
    // sound's mixing mode. The other console's model plays the new file's sound, and a synth plays a MIDI file. The
    // work's files are copied for the measure's thread. A sound that draws random numbers plays other notes on each
    // side, and the lines say so. A sound that the zones don't show plays as the work read it, in its mixing mode.
    job.cancel_measure = std::make_shared<std::atomic<bool>>(false);
    bool random = false;
    const format::File* document = Document();
    const bool other = job.done->played_sequence != nullptr;
    const std::optional<uint32_t> sound = other ? std::optional<uint32_t>(job.item->index) : ShownSound();
    const std::optional<uint32_t> sequence = sound ? SequenceOfSound(*sound) : std::nullopt;
    const std::shared_ptr<const song::Sequence> file_sequence = other ? job.done->sequence : sequence_;
    if (document != nullptr && sequence && file_sequence)
    {
        try
        {
            random = edit::DrawsRandom(*document, *sequence, *file_sequence);
        }
        catch (const std::exception&)
        {
            // A sequence whose commands the List Editor can't list measures without the line.
        }
    }

    // A sound on another console's engine plays its conversion. A sound of the Wii, and a sound on the Wii's engine,
    // render on the Wii's model. On the file's engine, a song of the GBA or of Konami's driver renders on the GBA's
    // model, and a sound of DSE on the DS's model. A sequence of the DS, and a sound on the DS's engine, render on the
    // DS's model.
    const song::Played played = other ? job.done->played : engine_sound_ ? engine_sound_->played : played_;
    const std::shared_ptr<const song::Sequence> played_sequence = other ? job.done->played_sequence
                                                                  : engine_sound_ && engine_sequence_ ? engine_sequence_
                                                                                                      : sequence_;
    const song::Mixing mixing = other && document != nullptr ? PlayedMixing(*document, *sound) : ShownMixing();
    const std::optional<format::Engine> to = SongEngineOf(job.Target());
    const PlayedSource source{played, played_sequence, mixing};
    if (to)
    {
        const auto work = [target = job.done->archive, source, random, to = *to,
                           model = std::string("The ") + EngineName(*to) + " model",
                           cancel = job.cancel_measure]() mutable
        {
            return MeasureOtherConsole(std::move(target), source, random, to, model, cancel);
        };
        job.measure = std::async(std::launch::async, work);
        return;
    }

    const auto instruments =
        job.Target() == ExportTarget::kMidiSoundFont ? measure::Instruments::kSoundFont : measure::Instruments::kSfz;
    const auto work = [conversion = *job.done->conversion, instruments, source, random, cancel = job.cancel_measure]()
    {
        return MeasureMidi(conversion, instruments, source, random, cancel);
    };
    job.measure = std::async(std::launch::async, work);
}

} // namespace hokuto::app
