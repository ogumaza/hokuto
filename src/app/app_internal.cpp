// SPDX-License-Identifier: MIT

#include "app/app_internal.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <span>
#include <utility>

#include "ntdlib/rvl/snd/rsar_options.h"

#include "edit/bytecode.h"
#include "edit/effects.h"
#include "edit/midi_imports.h"
#include "edit/midi_songs.h"
#include "edit/other_console.h"
#include "edit/replace.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/dse.h"
#include "song/gba.h"

namespace hokuto::app
{

std::string ReplaceAction(edit::FileKind kind)
{
    switch (kind)
    {
    case edit::FileKind::kBank:
        return "Replace Bank";
    case edit::FileKind::kWaveArchive:
        return "Replace Wave Archive";
    case edit::FileKind::kWaveSoundFile:
        break;
    }

    return "Replace Wave Sound File";
}

void TakeGroupSizes(std::future<std::vector<std::string>>& sizes, std::vector<std::string>& lines)
{
    if (!sizes.valid() || sizes.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }

    for (std::string& line : sizes.get())
    {
        lines.push_back(std::move(line));
    }
}

const PartView* SelectedPart(const std::optional<SongView>& song, const ArrangementState& state)
{
    if (!song || state.selected_part_track < 0 || state.selected_part_track >= static_cast<int>(song->tracks.size()))
    {
        return nullptr;
    }

    const TrackView& track = song->tracks[static_cast<std::size_t>(state.selected_part_track)];
    if (state.selected_part < 0 || state.selected_part >= static_cast<int>(track.parts.size()))
    {
        return nullptr;
    }

    return &track.parts[static_cast<std::size_t>(state.selected_part)];
}

bool SameArchive(const format::File& a, const format::File& b)
{
    const auto shared = [](const auto& x, const auto& y)
    {
        return x.SharesWith(y);
    };
    return a.native.SharesWith(b.native) &&
           std::equal(a.sequences.begin(), a.sequences.end(), b.sequences.begin(), b.sequences.end(), shared);
}

song::Mixing PlayedMixing(const format::File& file, uint32_t sound)
{
    const format::Engine engine = format::EngineOf(file, sound);
    const format::Engine played = edit::EngineRefused(file, engine) ? file.engine : engine;
    if (format::MixingRefused(played, format::Mixing::kEnhanced))
    {
        return song::Mixing::kStrict;
    }

    return edit::ModelMixing(format::MixingOf(file, sound));
}

std::optional<std::vector<std::optional<uint32_t>>> StartFieldsOf(const format::File& document,
                                                                  std::span<const uint8_t> bytes)
{
    try
    {
        return format::StartFieldsOfSounds(document, bytes);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

ntdlib::dse::Build DseBuildOfFile(const format::File& document)
{
    return document.native && document.native->cartridge ? song::DseBuildOf(*document.native->cartridge->image)
                                                         : ntdlib::dse::Build::kLater;
}

ntdlib::ntr::snd::Build DsBuildOfFile(const format::File& document)
{
    if (document.native && document.native->cartridge)
    {
        return song::DsBuildOf(*document.native->cartridge->image);
    }

    return ntdlib::ntr::snd::kDefaultBuild;
}

std::optional<song::DsGame> GameOf(const std::shared_ptr<const song::DsArchive>& archive)
{
    return archive ? archive->game : std::nullopt;
}

ReplaceSource SourceAt(std::shared_ptr<const format::File> file, song::Played played, uint32_t sound,
                       std::string file_name)
{
    ReplaceSource source;
    source.file = std::move(file);
    source.played = std::move(played);
    source.sound = sound;
    source.file_name = std::move(file_name);

    return source;
}

void ReadIncomingSequence(ReplaceSource& source, std::vector<uint8_t> bytes, const std::string& name,
                          const format::File& file)
{
    const song::Console console = edit::ConsoleOf(file);
    source.incoming =
        edit::IsMidi(bytes) ? edit::ReadIncomingMidi(bytes, name, console) : edit::ReadIncoming(bytes, console);
    if (edit::IsMidi(bytes))
    {
        source.midi_drums = edit::DrumNotesOf(bytes);
        source.midi = std::move(bytes);
    }
}

std::shared_ptr<const song::GbaRom> GbaRomOf(const format::File& version)
{
    return song::LoadGbaRom(format::PlayedBytes(version));
}

std::shared_ptr<const song::DriverRom> DriverRomOf(const format::File& version)
{
    return song::LoadDriverRom(version.native->engine, format::PlayedBytes(version));
}

std::shared_ptr<const song::DsArchive> DsArchiveOf(const format::File& version, std::optional<song::DsGame> game)
{
    return song::LoadDsArchive(format::ExportArchive(version), DsBuildOfFile(version), game);
}

std::shared_ptr<const song::DseSet> DseSetOf(const format::File& version)
{
    return song::LoadDseSet(format::ExportArchive(version), DseBuildOfFile(version));
}

std::shared_ptr<const ntdlib::rsar::SoundArchive> WiiArchiveOf(const format::File& version, const std::string& name)
{
    return song::LoadWiiArchive(format::ExportArchive(version), name, ntdlib::rvl::snd::kRsarOptions);
}

std::shared_ptr<const ntdlib::sar::SoundArchive> SarArchiveOf(const format::File& version, const std::string& name)
{
    return song::LoadArchive(format::ExportArchive(version), name, format::ExportGroupFiles(version));
}

} // namespace hokuto::app
