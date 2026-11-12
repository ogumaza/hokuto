// SPDX-License-Identifier: MIT

// The archives that the checks open, import and build (tools/checked_archive.h).

#include "checked_archive.h"

namespace hokuto::tools
{

CheckedArchive GbaChecked(std::vector<uint8_t> bytes)
{
    CheckedArchive archive;
    archive.gba = song::LoadGbaRom(std::move(bytes));
    const int count = std::max(archive.gba->info.song_count, 0);
    archive.gba_plays.assign(static_cast<std::size_t>(count), false);
    for (int song = 0; song < count; song++)
    {
        archive.names.push_back(song::GbaSongName(*archive.gba, song));
    }
    for (const int song : song::GbaSongs(*archive.gba))
    {
        if (song >= 0 && song < count)
        {
            archive.gba_plays[static_cast<std::size_t>(song)] = true;
        }
    }

    return archive;
}

CheckedArchive DriverChecked(format::Engine engine, std::vector<uint8_t> bytes)
{
    CheckedArchive archive;
    archive.driver = song::LoadDriverRom(engine, std::move(bytes));
    const uint32_t count = archive.driver->SoundCount();
    archive.gba_plays.assign(count, false);
    archive.names.resize(count);
    for (const uint32_t sound : archive.driver->Sounds())
    {
        archive.gba_plays[sound] = true;
        archive.names[sound] = archive.driver->SoundName(sound);
    }

    return archive;
}

CheckedArchive DsChecked(std::vector<uint8_t> sdat, const ntdlib::ntr::snd::Build& build,
                         std::optional<song::DsGame> game)
{
    CheckedArchive archive;
    archive.ds = song::LoadDsArchive(std::move(sdat), build, std::move(game));
    for (uint32_t sound = 0; sound < archive.ds->sounds.size(); sound++)
    {
        archive.names.push_back(song::DsSoundName(*archive.ds, sound));
    }

    return archive;
}

CheckedArchive DseChecked(std::span<const uint8_t> set, ntdlib::dse::Build build)
{
    CheckedArchive archive;
    archive.dse = song::LoadDseSet(set, build);
    for (uint32_t sound = 0; sound < archive.dse->sounds.size(); sound++)
    {
        archive.names.push_back(song::DseSoundName(*archive.dse, sound));
        for (const std::shared_ptr<const ntdlib::dse::WaveBank>& bank : archive.dse->sounds[sound].banks)
        {
            if (std::find(archive.dse_banks.begin(), archive.dse_banks.end(), bank) == archive.dse_banks.end())
            {
                archive.dse_banks.push_back(bank);
            }
        }
    }

    return archive;
}

bool IsDseImage(std::span<const uint8_t> bytes)
{
    return format::IsDsImage(bytes) && format::DsArchivesIn(bytes).empty() && format::HoldsDseFiles(bytes);
}

ntdlib::ntr::snd::Build DsBuildOfChecked(const format::File& file)
{
    return file.native && file.native->cartridge ? song::DsBuildOf(*file.native->cartridge->image)
                                                 : ntdlib::ntr::snd::kDefaultBuild;
}

bool IsWiiArchive(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0;
}

std::optional<ntdlib::SndBuild> GameBuildOf(const std::filesystem::path& path)
{
    const std::optional<std::filesystem::path> dol = song::FindMainDol(path);
    std::vector<uint8_t> code;
    return dol && xsflib::ReadFile(*dol, code) ? song::ReadWiiBuild(code) : std::nullopt;
}

std::string BuildText(const std::optional<ntdlib::SndBuild>& build)
{
    return !build                 ? "the build that the archive's version gives"
           : build->date > 0      ? std::to_string(build->date)
           : build->October2006() ? "2006"
                                  : "a build without a banner";
}

CheckedArchive OpenChecked(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    if (format::IsGbaRom(bytes))
    {
        std::vector<uint8_t> rom(bytes.begin(), bytes.end());
        const format::Driver* driver = format::DriverOfRom(bytes);
        return driver ? DriverChecked(driver->Facts().engine, std::move(rom)) : GbaChecked(std::move(rom));
    }
    if (IsDseImage(bytes))
    {
        const format::File file = format::ImportDseImage(bytes);
        return DseChecked(format::ExportArchive(file), song::DseBuildOf(*file.native->cartridge->image));
    }
    if (format::IsDsImage(bytes) || format::IsDsArchive(bytes))
    {
        const format::File file =
            format::IsDsImage(bytes) ? format::ImportDsImage(bytes) : format::ImportDsArchive(bytes);
        return DsChecked(format::ExportArchive(file), DsBuildOfChecked(file), edit::DsGameOfFile(file));
    }
    if (IsWiiArchive(bytes))
    {
        CheckedArchive archive;
        archive.wii = song::LoadWiiArchive(std::vector<uint8_t>(bytes.begin(), bytes.end()),
                                           xsflib::Utf8(path.filename()), ntdlib::rvl::snd::kRsarOptions);
        archive.build = GameBuildOf(path);
        return archive;
    }

    CheckedArchive archive;
    archive.sar = song::OpenArchive(path);
    return archive;
}

void TakeFillerOption(std::vector<std::string>& arguments)
{
    filler_runs = std::erase(arguments, std::string("--filler")) > 0;
}

format::File WithFillerChoice(format::File file)
{
    if (filler_runs && file.native && (file.native->engine == format::Engine::kGba || file.native->driver))
    {
        file.native.Edit([](format::NativeArchive& native) { native.gba_filler = true; });
    }

    return file;
}

format::File ImportChecked(std::span<const uint8_t> bytes, const CheckedArchive& archive,
                           std::vector<std::vector<uint8_t>> group_files)
{
    if (archive.dse)
    {
        return ntdlib::dse::IsSet(bytes) ? format::ImportDseSet(bytes) : format::ImportDseImage(bytes);
    }
    if (archive.ds)
    {
        return format::IsDsImage(bytes) ? format::ImportDsImage(bytes) : format::ImportDsArchive(bytes);
    }

    return archive.gba      ? WithFillerChoice(format::ImportGbaRom(bytes))
           : archive.driver ? WithFillerChoice(format::DriverOf(archive.driver->Rules().DriverEngine())->Import(bytes))
           : archive.wii ? format::ImportWiiArchive(bytes, archive.build)
                         : format::ImportArchive(bytes, archive.sar->ReadOptions().flavor, std::move(group_files), {},
                                                 archive.sar->GroupFileGroups());
}

format::File ImportChecked(std::span<const uint8_t> bytes, const CheckedArchive& archive)
{
    return ImportChecked(bytes, archive, archive.sar ? archive.sar->GroupFiles() : std::vector<std::vector<uint8_t>>());
}

CheckedArchive BuildChecked(const format::File& file, const std::string& name)
{
    std::vector<uint8_t> bytes = format::ExportArchive(file);
    if (file.native && file.native->engine == format::Engine::kGba)
    {
        return GbaChecked(std::move(bytes));
    }
    if (file.native && file.native->driver)
    {
        return DriverChecked(file.native->engine, std::move(bytes));
    }
    if (file.native && file.native->engine == format::Engine::kDs)
    {
        return DsChecked(std::move(bytes), DsBuildOfChecked(file), edit::DsGameOfFile(file));
    }
    if (file.native && file.native->engine == format::Engine::kDse)
    {
        return DseChecked(bytes, file.native->cartridge ? song::DseBuildOf(*file.native->cartridge->image)
                                                        : ntdlib::dse::Build::kLater);
    }
    if (file.native && file.native->engine == format::Engine::kWii)
    {
        CheckedArchive archive;
        archive.wii = song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions);
        archive.build = file.native->wii_build;
        return archive;
    }

    CheckedArchive archive;
    archive.sar = song::LoadArchive(bytes, name, format::ExportGroupFiles(file));
    return archive;
}

std::vector<std::optional<uint32_t>> SequencesOfChecked(const format::File& file, std::span<const uint8_t> bytes,
                                                        const CheckedArchive& archive)
{
    if (archive.ds)
    {
        return format::DsSoundSequences(file, format::ExportArchive(file));
    }
    if (archive.dse)
    {
        return format::DseSoundSequences(file, format::ExportArchive(file), song::DseSoundRefs(*archive.dse));
    }
    if (archive.gba)
    {
        std::vector<std::optional<uint32_t>> sequences;
        for (std::size_t song = 0; song < archive.names.size(); song++)
        {
            sequences.push_back(format::GbaSongSequence(file, archive.gba->info.song_table, static_cast<int>(song)));
        }
        return sequences;
    }
    if (archive.driver)
    {
        std::vector<std::optional<uint32_t>> sequences(archive.names.size());
        const std::vector<uint64_t> starts = format::NativePieceStarts(file);
        for (const uint32_t sound : archive.driver->Sounds())
        {
            const std::optional<uint64_t> place = archive.driver->FieldsOf(sound).sequence;
            sequences[sound] = place ? format::SequenceNamedAt(file, *place, starts) : std::nullopt;
        }
        return sequences;
    }

    return archive.wii ? format::SequencesOfFiles(file, bytes, *archive.wii)
                       : format::SequencesOfFiles(file, bytes, *archive.sar);
}

std::vector<std::optional<edit::SoundPlace>> PlacesOfChecked(const format::File& file, std::span<const uint8_t> bytes,
                                                             const CheckedArchive& archive)
{
    const std::vector<std::optional<uint32_t>> sequences = SequencesOfChecked(file, bytes, archive);
    if (archive.ds || archive.dse)
    {
        return edit::DsSoundPlaces(sequences);
    }
    if (archive.driver)
    {
        return edit::DriverSoundPlaces(file, *archive.driver, sequences);
    }
    if (archive.gba)
    {
        return edit::GbaSoundPlaces(sequences,
                                    format::GbaTrackFields(file, archive.gba->info.song_table, sequences.size()),
                                    song::GbaPlayerTracks(*archive.gba, sequences.size()));
    }
    const std::vector<std::optional<uint32_t>> starts = format::StartFieldsOfSounds(file, bytes);
    return archive.wii ? edit::SoundPlaces(*archive.wii, sequences, starts)
                       : edit::SoundPlaces(*archive.sar, sequences, starts);
}

SequenceSounds ReadSequenceSounds(const CheckedArchive& archive,
                                  std::span<const std::optional<edit::SoundPlace>> places, const std::string& only,
                                  std::map<uint32_t, song::Sequence>& songs)
{
    SequenceSounds sounds;
    for (uint32_t sound = 0; sound < places.size(); sound++)
    {
        if (!places[sound])
        {
            continue;
        }

        try
        {
            if (!only.empty() && archive.NameOf(sound) != only)
            {
                continue;
            }
            songs.emplace(sound, archive.Read(sound));
            sounds.by_sequence[places[sound]->sequence].push_back(sound);
            ++sounds.read;
        }
        catch (const std::exception&)
        {
            ++sounds.unreadable;
        }
    }

    return sounds;
}

void TakeModelOption(std::vector<std::string>& arguments)
{
    model_form = std::erase(arguments, std::string("--model")) > 0;
}

format::File CheckedForm(format::File file, std::span<const uint8_t> bytes)
{
    if (!model_form)
    {
        return file;
    }

    const std::vector<std::vector<uint8_t>> group_files = format::ExportGroupFiles(file);
    format::File model = format::InModelForm(std::move(file));
    if (!std::ranges::equal(format::ExportArchive(model), bytes) || format::ExportGroupFiles(model) != group_files)
    {
        throw std::runtime_error("the archive that the model form builds differs from the game's");
    }

    return model;
}

std::optional<EditOptions> ParseEditOptions(std::vector<std::string> arguments, const char* usage)
{
    TakeModelOption(arguments);
    TakeFillerOption(arguments);
    const bool timelines = arguments.size() > 1 && arguments[1] == "--timelines";
    const std::size_t first = timelines ? 2 : 1;
    if (arguments.size() < first + 1 || arguments.size() > first + 2)
    {
        std::fputs(usage, stderr);
        return std::nullopt;
    }
    const std::string only = arguments.size() > first + 1 ? arguments[first + 1] : std::string();

    return EditOptions{timelines, xsflib::PathFromUtf8(arguments[first]), only};
}

} // namespace hokuto::tools
