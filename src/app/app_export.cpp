// SPDX-License-Identifier: MIT

// File > Export: the work that makes an archive, a ROM, a song or another console's file, and the Export window.
// app_export_midi.cpp contains the MIDI export, app_export_measure.cpp the measure, and app_export_write.cpp the
// writing of the files.

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <utility>

#include "ntdlib/rvl/snd/rsar_options.h"
#include "seqlib/file_name.h"
#include "xsflib/files.h"

#include "app/app.h"
#include "app/app_export_parts.h"
#include "app/app_internal.h"
#include "app/engine_names.h"
#include "app/export_warnings.h"
#include "audio/clip.h"
#include "edit/other_console.h"
#include "edit/timelines.h"
#include "format/archive.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "song/archive.h"
#include "song/driver.h"
#include "song/items.h"
#include "song/streams.h"

namespace hokuto::app
{

using export_parts::Converts;

namespace
{

// The Export window's option of a song for the 3DS whose sound plays stereo waves (edit::MonoWaves), and its tooltip.
constexpr char kMonoWaves[] = "Convert stereo waves to mono";
constexpr char kMonoWavesTip[] =
    "Mixes each stereo wave down to mono. The wave then uses one of the 24 channels of the 3DS instead of two, and "
    "the 3DS drops fewer notes. Regions that play the wave play louder to keep the power of both channels. Hokuto "
    "turns this on when the song requires more channels at once than the 3DS has.";

// The Export window's option of a GBA ROM without room for the songs that their edits make longer
// (format::NativeArchive::gba_filler), and its tooltip.
constexpr char kFillerRuns[] = "Move songs into filler runs";
constexpr char kFillerRunsTip[] =
    "Moves songs that don't fit after the ROM data into filler runs: 16 KiB or more of 0xFF bytes inside the ROM that "
    "the music driver never reads. Other game code might still read them. When this is off, edits to those songs are "
    "left out of the ROM. The file saves this setting.";
constexpr char kDriverFillerRunsTip[] =
    "Moves songs that don't fit after the ROM data into filler runs: 4 KiB or more of 0xFF or zero bytes inside the "
    "ROM that the music driver never reads. Other game code might still read them. When this is off, edits to those "
    "songs are left out of the ROM. The file saves this setting.";

// The warnings about the notes that the edits of `version`, a file of a GBA ROM, make collide on the console's
// channels, against `original`, the file as it opened (CollisionWarnings). `rom` contains the ROM that `version`
// exports. Only the songs whose sequences or voices the edits changed are read again, from both ROMs. A voice's
// envelope and length decide when its channel comes free.
std::vector<std::string> GbaCollisionWarnings(const format::File& version, const format::File& original,
                                              const std::vector<uint8_t>& rom)
{
    if (SameArchive(version, original))
    {
        return {};
    }

    const std::shared_ptr<const song::GbaRom> after = song::LoadGbaRom(rom);
    const std::shared_ptr<const song::GbaRom> before = GbaRomOf(original);
    std::vector<int> changed;
    for (const int song : song::GbaSongs(*after))
    {
        const std::optional<uint32_t> now = format::GbaSongSequence(version, after->info.song_table, song);
        const std::optional<uint32_t> then = format::GbaSongSequence(original, before->info.song_table, song);
        if (now && then && *now < version.sequences.size() && *then < original.sequences.size() &&
            !version.sequences[*now].SharesWith(original.sequences[*then]) &&
            *version.sequences[*now] != *original.sequences[*then])
        {
            changed.push_back(song);
        }
    }
    const std::vector<int> voiced = song::GbaSongsWithOtherVoices(*before, *after);
    changed.insert(changed.end(), voiced.begin(), voiced.end());
    std::ranges::sort(changed);
    changed.erase(std::ranges::unique(changed).begin(), changed.end());

    std::vector<std::pair<std::string, song::Collisions>> added;
    for (const song::AddedCollisions& song : song::AddedGbaCollisions(*before, *after, changed))
    {
        added.emplace_back(song::GbaSongName(*after, song.song), song.added);
    }

    return CollisionWarnings(added, "the GBA");
}

// What the export's warnings say about the sounds that collide more often on the DS's channels than they did in
// `original`, the file as it opened (CollisionWarnings). `archive` contains the archive that `version` exports. Only
// the sounds that the edits can change are read again, up to a limit.
std::vector<std::string> DsCollisionWarnings(const format::File& version, const format::File& original,
                                             const std::vector<uint8_t>& archive)
{
    // CollisionsOf reads a GBA song or a DS sound. A set of DSE files gets no such warnings.
    constexpr std::size_t kMostSounds = 1024;
    if (version.native->engine == format::Engine::kDse || SameArchive(version, original))
    {
        return {};
    }

    const ntdlib::ntr::snd::Build build = DsBuildOfFile(version);
    const std::shared_ptr<const song::DsArchive> after = song::LoadDsArchive(archive, build);
    const std::shared_ptr<const song::DsArchive> before = song::LoadDsArchive(format::ExportArchive(original), build);
    std::vector<uint32_t> changed = song::DsSoundsThatChange(*before, *after);
    const std::size_t count = changed.size();
    changed.resize(std::min(count, kMostSounds));

    std::vector<std::pair<std::string, song::Collisions>> added;
    for (const song::AddedCollisions& sound : song::AddedDsCollisions(*before, *after, changed))
    {
        added.emplace_back(song::DsSoundName(*after, static_cast<uint32_t>(sound.song)), sound.added);
    }
    std::vector<std::string> warnings = CollisionWarnings(added, "the DS");
    if (count > kMostSounds)
    {
        warnings.push_back("The edits change " + std::to_string(count) + " sounds. Hokuto checked only the first " +
                           std::to_string(kMostSounds) + " for such notes.");
    }

    return warnings;
}

// The streams of `version` that are missing from the game.
std::vector<uint32_t> NewStreams(const std::shared_ptr<const format::File>& version)
{
    std::vector<uint32_t> streams;
    for (const auto& [id, stream] : version->native->streams)
    {
        if (!stream.from_game)
        {
            streams.push_back(id);
        }
    }

    return streams;
}

// Adds the `streams` of `version` to `work` as files beside the archive, at their places in the file table of the
// exported archive, `opened`, with the note that names them.
template <typename Opened>
void AddStreams(ExportWork& work, const std::shared_ptr<const format::File>& version,
                const std::vector<uint32_t>& streams, const Opened& opened)
{
    const std::vector<std::filesystem::path> paths = song::StreamPaths(opened, streams);
    std::vector<std::string> names;
    for (std::size_t i = 0; i < streams.size(); i++)
    {
        names.push_back(xsflib::Utf8(paths[i]));
        work.files.emplace_back(names.back(), *version->native->streams.at(streams[i]).bytes);
    }
    work.warnings.push_back(StreamsNote(names));
}

// The export of a GBA ROM, or a ROM with another of the GBA's drivers, into `work`.
void ExportRomWork(ExportWork& work, const std::shared_ptr<const format::File>& version,
                   const std::shared_ptr<const format::File>& original, const std::vector<std::string>& notes)
{
    const std::vector<uint32_t> moved = format::MovedSongs(*version);
    work.lacks_room = format::LacksRoom(*version);
    const bool filler = version->native->gba_filler;
    const format::File exported =
        work.lacks_room && !filler ? format::WithSongsOf(*version, *original, moved) : *version;
    if (work.lacks_room && !filler && format::LacksRoom(exported))
    {
        work.error = std::string(
                         "The ROM has no room after its data for the songs that no longer fit, even with their "
                         "edits left out. Turn on ") +
                     kFillerRuns + " instead.";
        return;
    }

    work.archive = format::ExportArchive(exported);
    const std::size_t before = version->native->game_sizes ? version->native->game_sizes->archive : work.archive.size();
    const RomRoom room = !work.lacks_room ? RomRoom::kFreeSpace : filler ? RomRoom::kFiller : RomRoom::kLeftOut;
    work.warnings = RomWarnings(moved.size(), before, work.archive.size(), room, version->native->driver.has_value());
    if (version->native->engine == format::Engine::kGba)
    {
        const std::vector<std::string> collisions = GbaCollisionWarnings(exported, *original, work.archive);
        work.warnings.insert(work.warnings.end(), collisions.begin(), collisions.end());
    }
    work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
}

// The DSi's digest keys in the key file at `dsi_key`. A file that can't be read or used adds a warning to `work` and
// sets `key_failed`.
std::optional<ntdlib::ntr::DsiKeys> ReadDsiKeys(const std::string& dsi_key, ExportWork& work, bool& key_failed)
{
    std::optional<ntdlib::ntr::DsiKeys> keys;
    std::vector<uint8_t> bytes;
    try
    {
        if (!xsflib::ReadFile(xsflib::PathFromUtf8(dsi_key), bytes))
        {
            throw std::invalid_argument("Can't read the key file.");
        }
        keys = format::DsiKeysOf(bytes);
    }
    catch (const std::exception& error)
    {
        work.warnings.push_back(std::string("Can't use the DSi digest key from Studio Setup. ") + error.what());
        key_failed = true;
    }

    return keys;
}

// The export of the archive of a DS file or of DSE's files into `work`: into the game's cartridge image, or alone.
void ExportDsWork(ExportWork& work, const std::shared_ptr<const format::File>& version,
                  const std::shared_ptr<const format::File>& original, const std::vector<std::string>& notes,
                  const std::string& dsi_key)
{
    const std::vector<uint8_t> archive = format::ExportArchive(*version);
    if (version->native->game_sizes)
    {
        work.warnings = version->native->engine == format::Engine::kDse
                            ? DseGrowthWarnings(version->native->game_sizes->archive, archive.size())
                            : GrowthWarnings({version->native->game_sizes->archive, archive.size(), {}}, false);
    }
    if (!version->native->cartridge)
    {
        std::vector<std::string> collisions = DsCollisionWarnings(*version, *original, archive);
        work.warnings.insert(work.warnings.end(), collisions.begin(), collisions.end());
        work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
        work.archive = archive;
        return;
    }

    std::optional<ntdlib::ntr::DsiKeys> keys;
    bool key_failed = false;
    const bool dsi = format::IsDsiImage(*version->native->cartridge->image);
    if (dsi && !dsi_key.empty())
    {
        keys = ReadDsiKeys(dsi_key, work, key_failed);
    }
    std::vector<std::string> collisions = DsCollisionWarnings(*version, *original, archive);
    work.warnings.insert(work.warnings.end(), collisions.begin(), collisions.end());
    format::DsImageExport exported = format::ExportDsImage(*version, keys);
    if (exported.image != format::CartridgeImage(*version->native->cartridge))
    {
        if (dsi)
        {
            work.warnings.push_back(DsiNote(exported.stale, key_failed));
        }
        if (const std::optional<std::string> signatures = SignatureNote(exported.stale, dsi))
        {
            work.warnings.push_back(*signatures);
        }
    }
    std::vector<std::string> growth = DsImageWarnings(version->native->cartridge->size, exported.image.size());
    work.warnings.insert(work.warnings.end(), growth.begin(), growth.end());
    work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
    work.archive = std::move(exported.image);
}

// The export of an archive of the Wii into `work`, with its new `streams` beside it.
void ExportWiiWork(ExportWork& work, const std::shared_ptr<const format::File>& version,
                   const std::vector<std::string>& notes, const std::vector<uint32_t>& streams)
{
    work.archive = format::ExportArchive(edit::FoldTimelines(*version));
    if (version->native->game_sizes)
    {
        work.warnings = GrowthWarnings({version->native->game_sizes->archive, work.archive.size(), {}}, false);
    }
    if (!streams.empty())
    {
        AddStreams(work, version, streams, *song::LoadWiiArchive(work.archive, "", ntdlib::rvl::snd::kRsarOptions));
    }
    work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
}

// The note that names the group files that go beside the archive, `group_names`, at their places. `source_folder` is
// the folder of the file that the user opened.
std::string GroupFilesWarning(const std::vector<std::string>& group_names, const std::filesystem::path& source_folder)
{
    // A group file whose game's file lies in the userManagementFiles folder beside the file that the
    // user opened goes into that folder (song::PlaceGroupFiles).
    std::vector<std::pair<std::string, std::vector<uint8_t>>> named;
    for (const std::string& name : group_names)
    {
        named.emplace_back(name, std::vector<uint8_t>());
    }
    if (!source_folder.empty())
    {
        named = song::PlaceGroupFiles(std::move(named), std::span(&source_folder, 1));
    }
    std::vector<std::string> placed;
    for (const auto& [name, bytes] : named)
    {
        placed.push_back(name);
    }

    return GroupFilesNote(placed);
}

// The export of an archive of the later consoles into `work`, with the group files that the edits changed and its
// new `streams` beside it. The group files go into `source_folder` where the game keeps them there.
void ExportSarWork(ExportWork& work, const std::shared_ptr<const format::File>& version,
                   const std::shared_ptr<const format::File>& original, const std::vector<std::string>& notes,
                   const std::vector<uint32_t>& streams, const std::filesystem::path& source_folder)
{
    const ntdlib::sar::Flavor flavor = FlavorOf(*version->native);
    const format::File folded = edit::FoldTimelines(*version);
    work.archive = format::ExportArchive(folded);
    std::optional<format::GameSizes> before = version->native->game_sizes;
    if (!before && !SameArchive(*version, *original))
    {
        const format::File folded_original = edit::FoldTimelines(*original);
        before =
            format::SizesOf(format::ExportArchive(folded_original), flavor, format::GroupFilesBeside(folded_original));
    }
    if (before)
    {
        work.warnings = GrowthWarnings(
            format::MeasureGrowth(*before, work.archive, flavor, format::GroupFilesBeside(folded)), true);
    }

    // The group files that the edits changed go beside the archive once the user chooses its folder.
    std::vector<std::string> group_names;
    for (format::ChangedGroupFile& changed : format::ChangedGroupFiles(folded))
    {
        if (!changed.group)
        {
            work.warnings.push_back(UnknownGroupNote(changed.index));
            continue;
        }

        const std::string group = changed.name.empty() ? "GROUP_" + std::to_string(*changed.group) : changed.name;
        group_names.push_back(ntdlib::sar::GroupFileName(seqlib::SafeFileName(group), flavor));
        work.group_files.push_back(std::move(changed));
    }
    if (!group_names.empty())
    {
        work.warnings.push_back(GroupFilesWarning(group_names, source_folder));
    }

    if (!streams.empty())
    {
        AddStreams(work, version, streams, *song::LoadArchive(work.archive, "", format::ExportGroupFiles(folded)));
    }

    work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
}

} // namespace

namespace export_parts
{

bool Converts(const format::File& file, uint32_t sound)
{
    const format::Engine engine = format::EngineOf(file, sound);
    return engine != file.engine && !edit::EngineRefused(file, engine);
}

} // namespace export_parts

void App::StartExport(ExportTarget target)
{
    const format::File* document = Document();
    const bool from_sequence = target != ExportTarget::kSoundArchive;
    const bool midi = target == ExportTarget::kMidiSoundFont || target == ExportTarget::kMidiSfz;
    // A song of the GBA's drivers is exported as MIDI with SoundFont 2 or to another console. A driver whose conversion
    // writes SFZ files also offers MIDI with SFZ. A DS sequence is exported as MIDI, or to another console, and a sound
    // of DSE as MIDI.
    if (export_ || !history_ || preview_ || document == nullptr || !document->native ||
        (from_sequence && (!sequence_ || (!ArchiveOpen() || !archive_version_))) ||
        (played_.Wii() && from_sequence && !midi && !SongEngineOf(target)) ||
        ((played_.Gba() || played_.Driver()) && from_sequence && target != ExportTarget::kMidiSoundFont &&
         !(target == ExportTarget::kMidiSfz && played_.Driver() && played_.Driver()->WritesSfz()) &&
         !SongEngineOf(target)) ||
        ((played_.Ds() || played_.Dse()) && from_sequence && !midi && !SongEngineOf(target)))
    {
        return;
    }

    // The work exports the version that the history has reached.
    Export job;
    job.view = SongEngineOf(target) ? OtherConsoleExportView(document->engine, sequence_->name)
                                    : FileExportView(target, from_sequence ? sequence_->name : "");
    if (!from_sequence)
    {
        // An imported archive keeps its file's name.
        const std::filesystem::path source = xsflib::PathFromUtf8(import_path_.empty() ? document_path_ : import_path_);
        const bool rom = document->native->engine == format::Engine::kGba || document->native->driver;
        const char* extension = rom                                                        ? ".gba"
                                : document->native->cartridge                              ? ".nds"
                                : document->native->engine == format::Engine::kDs          ? ".sdat"
                                : document->native->engine == format::Engine::kWii         ? ".brsar"
                                : FlavorOf(*document->native) == ntdlib::sar::Flavor::kCtr ? ".bcsar"
                                                                                           : ".bfsar";
        if (rom)
        {
            job.view.title = "Export ROM";
            job.view.lines = {"ROM (rebuilt with edits)"};
        }
        else if (document->native->cartridge)
        {
            job.view.title = "Export Image";
            job.view.lines = {document->native->engine == format::Engine::kDse
                                  ? "Cartridge image (with edited sound files)"
                                  : "Cartridge image (with edited sound archive)"};
        }
        job.file_name =
            import_path_.empty() ? xsflib::Utf8(source.stem()) + extension : xsflib::Utf8(source.filename());
        job.source_folder = source.parent_path();
        job.extension = extension;
        StartArchiveWork(job);
    }
    else if (SongEngineOf(target))
    {
        StartSongWork(job);
    }
    else if (played_.Gba())
    {
        StartGbaMidiWork(job, sequence_->sound);
    }
    else if (played_.Ds())
    {
        StartDsMidiWork(job, sequence_->sound);
    }
    else if (played_.Dse())
    {
        StartDseMidiWork(job, sequence_->sound);
    }
    else if (played_.Driver())
    {
        StartDriverMidiWork(job, sequence_->sound);
    }
    else
    {
        StartMidiWork(job, sequence_->sound);
    }

    export_ = std::move(job);
}

std::vector<std::string> App::EngineSoundNames(const format::File& version) const
{
    std::vector<std::string> on_engines;
    for (const auto& [sound, engine] : version.sound_engines)
    {
        if (engine == version.engine)
        {
            continue;
        }

        if (version.native->tables && sound < version.native->tables->sounds.size())
        {
            on_engines.push_back(version.native->tables->sounds[sound].name);
        }
        else if (played_.Wii() && sound < played_.Wii()->Sounds().size())
        {
            on_engines.push_back(played_.Wii()->Sounds()[sound].name);
        }
        else if (played_.Sar() && sound < played_.Sar()->Sounds().size())
        {
            on_engines.push_back(played_.Sar()->Sounds()[sound].name);
        }
        else if (played_.Gba())
        {
            on_engines.push_back(song::GbaSongName(*played_.Gba(), static_cast<int>(sound)));
        }
        else if (played_.Ds())
        {
            on_engines.push_back(song::DsSoundName(*played_.Ds(), sound));
        }
        else if (played_.Dse())
        {
            on_engines.push_back(song::DseSoundName(*played_.Dse(), sound));
        }
        else if (played_.Driver())
        {
            on_engines.push_back(played_.Driver()->SoundName(sound));
        }
    }

    return on_engines;
}

void App::StartArchiveWork(Export& job)
{
    // The work exports the version that the history has reached. The game's sizes measure the growth. Without them,
    // the file as it was opened measures it, and a version that builds the same archive didn't grow. The streams that
    // the game lacks go beside the archive. The archive's file table gives their places.
    const std::shared_ptr<const format::File> version = history_->Current();
    const std::shared_ptr<const format::File> original = history_->Original();

    // A sound that plays on another console's engine in Hokuto goes out as the archive holds it. The file's tables
    // name it, or else the archive or the ROM that the zones play.
    const std::vector<std::string> on_engines = EngineSoundNames(*version);
    std::vector<std::string> notes;
    if (!on_engines.empty())
    {
        notes.push_back(EngineSoundsNote(on_engines, EngineName(version->engine)));
    }
    const auto build = [version, original, notes, source_folder = job.source_folder, dsi_key = settings_.dsi_key]
    {
        // The streams that the game lacks go beside the archive. The file table of the exported archive, `opened`,
        // gives their places.
        ExportWork work;
        const std::vector<uint32_t> streams = NewStreams(version);
        try
        {
            // A GBA ROM contains its songs among the game's code and data. The game's size measures its growth. A ROM
            // without room for the songs that outgrow their places follows the file's choice: the songs go into the
            // ROM's filler runs, or their edits stay out, and the songs go in as the file opened. A ROM with another of
            // the GBA's drivers does the same.
            if (version->native->engine == format::Engine::kGba || version->native->driver)
            {
                ExportRomWork(work, version, original, notes);
                return work;
            }

            // The archive of a DS file is written back into the game's cartridge image, or exported alone, and so are
            // DSE's files. The game's size measures the archive's growth. An image that also runs on the DSi gets new
            // hashes, computed with the DSi's digest key from Studio Setup. The notes name the hashes that no key
            // updates, such as the RSA signature, on the DS too.
            if (version->native->engine == format::Engine::kDs || version->native->engine == format::Engine::kDse)
            {
                ExportDsWork(work, version, original, notes, dsi_key);
                return work;
            }

            // An archive of the Wii keeps its groups inside. The game's size measures its growth.
            if (version->native->engine == format::Engine::kWii)
            {
                ExportWiiWork(work, version, notes, streams);
                return work;
            }

            ExportSarWork(work, version, original, notes, streams, source_folder);
        }
        catch (const std::exception& error)
        {
            work.error = std::string("Can't export the ") +
                         (version->native->engine == format::Engine::kGba || version->native->driver ? "ROM. "
                          : version->native->cartridge ? "image. "
                                                       : "sound archive. ") +
                         format::AsSentence(error.what());
        }

        return work;
    };
    job.work = std::async(std::launch::async, build);
}

void App::StartSongWork(Export& job)
{
    // The work sends the shown sequence's sound of the version into a new file of the target's console
    // (edit::ForOtherConsole, edit::ForWii for the Wii, or edit::ForDs for the DS). The sound is the one that a saved
    // file contains. The new file's archive comes along for the measure.
    job.file_name = seqlib::SafeFileName(sequence_->name) + ".hok";
    const format::Engine to = *SongEngineOf(job.Target());
    std::vector<format::Engine> others;
    for (const ExportTarget target : job.view.targets)
    {
        const std::optional<format::Engine> engine = SongEngineOf(target);
        if (engine && *engine != to)
        {
            others.push_back(*engine);
        }
    }
    const auto send = [version = history_->Current(), sound = sequence_->sound, to, others, mono = job.mono]
    {
        ExportWork work;
        std::optional<format::File> folded;
        const auto convert = [&folded, sound, mono](format::Engine engine)
        {
            if (engine == format::Engine::kDs)
            {
                return edit::ForDs(*folded, sound);
            }

            const std::optional<ntdlib::sar::Flavor> flavor = format::FlavorOf(engine);
            return flavor ? edit::ForOtherConsole(*folded, sound, *flavor, mono) : edit::ForWii(*folded, sound);
        };
        try
        {
            folded = edit::FoldTimelines(*version);
            const edit::OtherConsoleSong song = convert(to);
            work.files.emplace_back("", format::Write(song.file));
            work.archive = format::ExportArchive(edit::FoldTimelines(song.file));
            work.warnings = song.warnings;
            work.stereo_waves = song.stereo_waves;
            work.mono_waves = song.mono_waves;
        }
        catch (const std::exception& error)
        {
            // A reason such as commands that overlap holds for other consoles too. The message names each console of
            // the window's targets that refuses the sound for the same reason.
            std::vector<std::string> consoles = {std::string("the ") + EngineName(to)};
            for (const format::Engine other : folded ? others : std::vector<format::Engine>())
            {
                try
                {
                    convert(other);
                }
                catch (const std::exception& refusal)
                {
                    if (std::string_view(refusal.what()) == error.what())
                    {
                        consoles.push_back(std::string("the ") + EngineName(other));
                    }
                }
            }
            std::string named = consoles.front();
            for (std::size_t i = 1; i < consoles.size(); i++)
            {
                named += (i + 1 == consoles.size() ? " or " : ", ") + consoles[i];
            }
            work.error = "Can't send the sound to " + named + ". " + format::AsSentence(error.what());
        }

        return work;
    };
    job.work = std::async(std::launch::async, send);
}

void App::DrawExport()
{
    // A dropped export's work goes once it's done. So does a dropped measure's.
    std::erase_if(dropped_exports_, Done<ExportWork>);
    std::erase_if(dropped_measures_, Done<MeasuredView>);

    TakeExportWork();

    // The window shows the export until the files are written or the user cancels it.
    if (!export_)
    {
        CloseExportWindow();
        return;
    }

    // An export to MIDI or to the other console measures what it would sound like, once its files are made
    // (StartMeasure). Export… doesn't wait for the measure.
    const bool midi = export_->Target() == ExportTarget::kMidiSoundFont || export_->Target() == ExportTarget::kMidiSfz;
    const bool measures = (midi || SongEngineOf(export_->Target()).has_value()) && export_->view.refused.empty();
    if (measures && export_->done && (!midi || export_->done->conversion) && !export_->measure.valid() &&
        !export_->measured)
    {
        StartMeasure(*export_);
    }
    if (export_->measure.valid() && Done(export_->measure))
    {
        export_->measured = export_->measure.get();
    }
    MeasuredView measuring;
    measuring.status = "Measuring the export…";
    const MeasuredView* measured = !measures || !export_->done ? nullptr
                                   : export_->measured         ? &*export_->measured
                                                               : &measuring;

    // A choice of another target drops the work for the old one. A wave archive's WAV files and a bank's SFZ files go
    // into a folder.
    const ExportChoice choice =
        DrawExportWindow(export_->view, export_place_, export_->done ? &export_->done->warnings : nullptr, measured);
    if (choice == ExportChoice::kCancel)
    {
        DropExport();
    }
    else if (choice == ExportChoice::kTarget || choice == ExportChoice::kOption)
    {
        RestartExportWork(choice);
    }
    else if (choice == ExportChoice::kPlay && export_->measured &&
             export_->view.play < export_->measured->places.size())
    {
        const MeasuredPlace& place = export_->measured->places[export_->view.play];
        PlayAlone(std::make_unique<audio::Clip>(export_->view.play_hokuto ? place.hokuto_clip : place.export_clip,
                                                place.rate));
    }
    else if (choice == ExportChoice::kExport)
    {
        const bool folder = export_->Target() == ExportTarget::kSfz ||
                            (export_->item && export_->item->kind == song::ItemKind::kWaveArchive &&
                             export_->Target() == ExportTarget::kWav);
        ShowDialog(folder ? Dialog::kExportFolder : Dialog::kExport);
    }
}

void App::TakeExportWork()
{
    // The work's files and warnings, once it's done. A window with other targets shows an error in place of the
    // warnings, and the user can choose another target. A window with one target closes with a message. A song for the
    // 3DS whose sound plays stereo waves offers them as mono. The check box shows what the work did.
    if (export_ && !export_->done && Done(export_->work))
    {
        export_->done = export_->work.get();
        if (!export_->done->error.empty() && (export_->view.lines.size() > 1 || export_->done->lacks_room))
        {
            export_->view.refused = export_->done->error;
        }
        else if (!export_->done->error.empty())
        {
            const std::string error = export_->done->error;
            export_.reset();
            ShowMessage(error);
        }
        else if (export_->Target() == ExportTarget::kCtrSong && export_->done->stereo_waves)
        {
            export_->view.option = kMonoWaves;
            export_->view.option_tooltip = kMonoWavesTip;
            export_->view.option_on = export_->done->mono_waves;
        }
        // A ROM without room offers the filler runs, a refused export included: its refusal asks for them.
        if (export_ && export_->done->lacks_room)
        {
            export_->view.option = kFillerRuns;
            export_->view.option_tooltip = history_->Current()->native->driver ? kDriverFillerRunsTip : kFillerRunsTip;
            export_->view.option_on = history_->Current()->native->gba_filler;
        }
    }
}

void App::RestartExportWork(ExportChoice choice)
{
    // The work and the measure for the old target or option go. A song's target starts its measure again once the
    // new work is done. Another target starts without the option. The option sends a song to the 3DS again, with
    // its stereo waves in stereo or in mono. A GBA ROM's option changes the file's choice of filler runs with an
    // edit, and the work exports the new version.
    export_->view.refused.clear();
    const bool archive = export_->Target() == ExportTarget::kSoundArchive;
    if (choice == ExportChoice::kTarget)
    {
        export_->view.option.clear();
        export_->mono = edit::MonoWaves::kAuto;
    }
    else if (archive)
    {
        const bool on = export_->view.option_on;
        format::File next = *history_->Current();
        next.native.Edit([on](format::NativeArchive& native) { native.gba_filler = on; });
        Do("Change Filler Runs",
           "File: " + file_name_ + " - " + (on ? kFillerRuns : "Leave out edits of songs without room"),
           std::move(next));
    }
    else
    {
        export_->mono = export_->view.option_on ? edit::MonoWaves::kOn : edit::MonoWaves::kOff;
    }
    DropExportWork(*export_);
    export_->done.reset();
    export_->measured.reset();
    if (export_->item)
    {
        StartItemWork(*export_);
    }
    else if (archive)
    {
        StartArchiveWork(*export_);
    }
    else
    {
        StartSongWork(*export_);
    }
}

void App::DropExport()
{
    if (export_)
    {
        DropExportWork(*export_);
    }
    export_.reset();
}

void App::DropExportWork(Export& job)
{
    if (!job.done)
    {
        dropped_exports_.push_back(std::move(job.work));
    }
    if (job.measure.valid())
    {
        job.cancel_measure->store(true);
        dropped_measures_.push_back(std::move(job.measure));
    }
}

} // namespace hokuto::app
