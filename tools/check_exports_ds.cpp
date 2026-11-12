// SPDX-License-Identifier: MIT

// hokuto_check_exports: the checks of the DS's sound archives and of DSE's files.

#include "check_exports.h"

namespace hokuto::tools::exports
{

namespace
{

// The options of ntdlib's frame for the sequence `label` of `archive`, of `game`: the build of the archive's driver,
// and the settings and commands of the sequence's profile (ntdlib::ntr::GameProfileOf). ntdlib's DS converter uses the
// same options. The output effect stays out.
export_::ntr::PerformOptions DsReferenceOptions(const hokuto::song::DsArchive& archive, const std::string& label,
                                                const std::optional<hokuto::song::DsGame>& game)
{
    export_::ntr::PerformOptions options;
    options.build = archive.build;
    if (!game)
    {
        return options;
    }

    const ntdlib::ntr::GameProfile profile = ntdlib::ntr::GameProfileOf(game->code, game->path, game->index, label);
    const ntdlib::ntr::GameSettings& settings = profile.settings;
    options.player_channels.insert(settings.channels.begin(), settings.channels.end());
    options.track_channels = settings.track_channels;
    options.reverb = settings.reverb;
    options.commands = profile.commands;
    for (uint8_t i = 0; i < 16; i++)
    {
        if (settings.variables.local[i])
        {
            options.variables.try_emplace(i, *settings.variables.local[i]);
        }
        if (settings.variables.global[i])
        {
            options.variables.try_emplace(static_cast<uint8_t>(16 + i), *settings.variables.global[i]);
        }
    }

    return options;
}

// Checks the conversions of the sounds of `ours`, the archive that a Hokuto file builds, against those of `theirs`, the
// game's archive of `game`. ntdlib's frame of a DS converter makes both conversions. `name` names the archive, and
// `only` a sound to check alone. Both sides name `program` as the program that made the files.
Counts CheckDsSounds(const hokuto::song::DsArchive& theirs_archive, const std::optional<hokuto::song::DsGame>& game,
                     const hokuto::song::DsArchive& ours_archive, const std::string& name, const std::string& only,
                     const std::string& program)
{
    Counts counts;
    for (const uint32_t sound : hokuto::song::DsSounds(theirs_archive))
    {
        const std::string sound_name = hokuto::song::DsSoundName(theirs_archive, sound);
        if (!only.empty() && sound_name != only)
        {
            continue;
        }

        counts.sounds++;
        Side theirs;
        Side ours;
        try
        {
            export_::ntr::Conversion converted =
                export_::ntr::ConvertSequence(theirs_archive.archive, *theirs_archive.sounds[sound], sound_name,
                                              DsReferenceOptions(theirs_archive, sound_name, game), program);
            theirs.conversion = hokuto::song::FrameConversion(std::move(converted));
        }
        catch (const std::exception& error)
        {
            theirs.error = error.what();
        }
        try
        {
            ours.conversion = hokuto::song::ConvertToMidi(ours_archive, sound, {}, program);
        }
        catch (const std::exception& error)
        {
            ours.error = error.what();
        }

        CompareSides(theirs, ours, name, sound_name, counts);
    }

    return counts;
}

} // namespace

int CheckDs(std::vector<uint8_t> bytes, const std::string& name, const std::string& only, const std::string& program)
{
    // Each archive's file, and the game's archive with the build of the image's driver and the archive's game.
    struct Archive
    {
        std::string name;
        hokuto::format::File file;
        std::shared_ptr<const hokuto::song::DsArchive> theirs;
        std::optional<hokuto::song::DsGame> game;
    };
    std::vector<Archive> archives;
    try
    {
        if (hokuto::format::IsDsArchive(bytes))
        {
            hokuto::format::File file = hokuto::format::ImportDsArchive(bytes);
            archives.push_back({name, std::move(file), hokuto::song::LoadDsArchive(std::move(bytes)), std::nullopt});
        }
        else
        {
            const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(bytes);
            const ntdlib::ntr::snd::Build build = hokuto::song::DsBuildOf(bytes);
            const std::vector<hokuto::format::DsArchivePlace> places = hokuto::format::DsArchivesIn(bytes);
            for (auto place = places.begin(); place != places.end(); ++place)
            {
                // The places come in the order of the files and of the offsets in each.
                const auto earlier =
                    std::count_if(places.begin(), place, [&place](const hokuto::format::DsArchivePlace& other)
                                  { return other.file == place->file; });
                const std::span<const uint8_t> data = rom.FileData(place->file).subspan(place->offset, place->size);
                archives.push_back(
                    {name + ": " + place->path + " at " + std::to_string(place->offset),
                     hokuto::format::ImportDsImage(bytes, *place),
                     hokuto::song::LoadDsArchive({data.begin(), data.end()}, build),
                     hokuto::song::DsGame{rom.Header().game_code, place->path, static_cast<uint32_t>(earlier)}});
            }
        }
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    bool differ = false;
    for (const Archive& archive : archives)
    {
        std::shared_ptr<const hokuto::song::DsArchive> ours;
        try
        {
            ours = hokuto::song::LoadDsArchive(hokuto::format::ExportArchive(archive.file), archive.theirs->build,
                                               hokuto::edit::DsGameOfFile(archive.file));
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "%s: %s\n", archive.name.c_str(), error.what());
            return 2;
        }
        const Counts counts = CheckDsSounds(*archive.theirs, archive.game, *ours, archive.name, only, program);
        std::printf("%s: %zu sequences checked. %zu differ, and %zu fail on both sides.\n", archive.name.c_str(),
                    counts.sounds, counts.differ, counts.failed);
        differ = differ || counts.differ > 0;
    }

    return differ ? 1 : 0;
}

int CheckDse(const std::vector<uint8_t>& bytes, const std::string& name, const std::string& only,
             const std::string& program)
{
    std::shared_ptr<const hokuto::song::DseSet> theirs_set;
    std::shared_ptr<const hokuto::song::DseSet> ours_set;
    try
    {
        theirs_set = hokuto::song::LoadDseSet(bytes);
        ours_set = hokuto::song::LoadDseSet(hokuto::format::ExportArchive(hokuto::format::ImportDseImage(bytes)),
                                            hokuto::song::DseBuildOf(bytes));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), error.what());
        return 2;
    }

    Counts counts;
    for (const uint32_t sound : hokuto::song::DseSounds(*theirs_set))
    {
        const std::string sound_name = hokuto::song::DseSoundName(*theirs_set, sound);
        if (!only.empty() && sound_name != only)
        {
            continue;
        }

        counts.sounds++;
        Side theirs;
        Side ours;
        try
        {
            export_::dse::PerformOptions options;
            options.build = theirs_set->build;
            export_::dse::Conversion converted =
                export_::dse::ConvertSequence(theirs_set->sounds[sound].Files(), sound_name, options, program);
            theirs.conversion = hokuto::song::FrameConversion(std::move(converted));
        }
        catch (const std::exception& error)
        {
            theirs.error = error.what();
        }
        try
        {
            ours.conversion = hokuto::song::ConvertToMidi(*ours_set, sound, program);
        }
        catch (const std::exception& error)
        {
            ours.error = error.what();
        }

        CompareSides(theirs, ours, name, sound_name, counts);
    }
    std::printf("%s: %zu sounds of DSE checked. %zu differ, and %zu fail on both sides.\n", name.c_str(), counts.sounds,
                counts.differ, counts.failed);

    return counts.differ > 0 ? 1 : 0;
}

} // namespace hokuto::tools::exports
