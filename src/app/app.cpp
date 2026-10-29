// SPDX-License-Identifier: MIT

#include "app/app.h"

#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ntdlib/rsar/wave_file.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/decode.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/binary.h"
#include "xsflib/files.h"

#include "app/control_bar.h"
#include "app/lanes.h"
#include "app/markers.h"
#include "app/preview_song.h"
#include "app/recent_projects.h"
#include "app/sequence_view.h"
#include "app/system_fonts.h"
#include "app/system_ui.h"
#include "app/tempo_row.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "audio/clip.h"
#include "audio/test_tone.h"
#include "edit/bytecode.h"
#include "edit/commands.h"
#include "edit/converter.h"
#include "edit/effects.h"
#include "edit/groups.h"
#include "edit/instruments.h"
#include "edit/mix.h"
#include "edit/new_items.h"
#include "edit/notes.h"
#include "edit/other_console.h"
#include "edit/reach.h"
#include "edit/replace.h"
#include "edit/streams.h"
#include "edit/subroutines.h"
#include "edit/tables.h"
#include "edit/timelines.h"
#include "edit/tracks.h"
#include "edit/waves.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"
#include "measure/measure.h"
#include "song/aac.h"
#include "song/archive.h"
#include "song/frame_resampler.h"
#include "song/items.h"
#include "song/midi_export.h"
#include "song/preview.h"
#include "song/sequence_preview.h"
#include "song/streams.h"

namespace hokuto::app
{
namespace
{

using theme::Px;

// The ring holds about a third of a second at 48 kHz. The worker keeps 100 ms in it.
constexpr std::size_t kRingFrames = 16'384;
constexpr std::chrono::milliseconds kAhead{100};
constexpr std::size_t kBlockFrames = 256;

// The auditions' worker keeps a lead of two of the device's buffers in its ring, and 10 ms at least. The ring then
// holds a buffer's frames whenever the device takes them. A note's release comes up to a lead late.
constexpr std::chrono::milliseconds kLeastAuditionLead{10};
constexpr std::size_t kAuditionBlockFrames = 64;

// The rate of the ring's frames until a renderer sets another, and the test tone's rate. SDL3 converts it to the
// device's rate.
constexpr int kDefaultRate = 48'000;

// The window's size on a first run, before the display's scale.
constexpr float kDefaultWidth = 1440.0f;
constexpr float kDefaultHeight = 900.0f;

// After a peak, a meter falls back this many dB a second. Below -80 dB, a meter shows silence.
constexpr float kMeterFall = 24.0f;
constexpr float kMeterFloor = -80.0f;

// After each event, the window draws at the display's rate for a second. The time is in SDL3's nanoseconds. Dear
// ImGui's tooltips appear within it. So do the frames that Dear ImGui spreads a quick click across.
constexpr uint64_t kLinger = 1'000'000'000;

// The audio device rests after 30 s without sound (App::RestOutput). The timer that wakes the program for the rest
// comes 0.1 s after the rest is due. The times are in SDL3's nanoseconds.
constexpr uint64_t kOutputRest = 30'000'000'000;
constexpr uint64_t kOutputTimerSlack = 100'000'000;

// Without a sequence, a frame moves the transport on by the frame's time, up to 0.1 s. The first frame after a wait for
// events comes long after the frame before it.
constexpr float kLongestStep = 0.1f;

// The key that the Key Editor shows a track without notes round: middle C.
constexpr int kMiddleC = 60;

// Debug builds turn on the GPU's validation.
#ifdef NDEBUG
constexpr bool kGpuDebug = false;
#else
constexpr bool kGpuDebug = true;
#endif

constexpr char kSystemDefault[] = "System Default";
constexpr char kStudioSetup[] = "Studio Setup";
constexpr char kProjectSetup[] = "Project Setup";
constexpr char kNewProject[] = "New Project";
constexpr char kPreferences[] = "Preferences";

// The ID of the window that waits while a file opens. The window's title is the file's name followed by the ID. Dear
// ImGui shows the name, and the window keeps the same ID for every file.
constexpr char kOpening[] = "###Opening";

// The name of a new file before its first save.
constexpr char kUntitled[] = "Untitled";

// The first name of a sound that Project > Add Sequence Sound… adds.
constexpr char kNewSoundName[] = "SEQ_NEW";
constexpr char kNewBankName[] = "BANK_NEW";

// The consoles of a new file in the New Project window, in the order of format::Engine.
constexpr std::array<const char*, 3> kEngineNames = {"Switch", "Wii U", "3DS"};

// The name of the console whose rules `engine` follows, such as "Wii U", for Project Setup, the mixer and the messages.
const char* EngineName(format::Engine engine)
{
    switch (engine)
    {
    case format::Engine::kWiiU:
        return "Wii U";
    case format::Engine::k3ds:
        return "3DS";
    case format::Engine::kWii:
        return "Wii";
    case format::Engine::kSwitch:
        break;
    }

    return "Switch";
}

// The build of NW4R SND that a Wii file plays by, and where it came from, for Project Setup. Without the game's
// main.dol, the archive's version gives a build of 2007 or of June 2008.
std::string WiiBuildText(const format::NativeArchive& native)
{
    constexpr std::array<const char*, 12> kMonths = {"January",   "February", "March",    "April",
                                                     "May",       "June",     "July",     "August",
                                                     "September", "October",  "November", "December"};
    const ntdlib::SndBuild build = format::BuildOfWiiArchive(native);
    if (!native.wii_build)
    {
        return std::string("NW4R SND of ") + (build.SinceMarch2008() ? "June 2008" : "2007") +
               ", by the archive's version";
    }
    if (build.date == 0)
    {
        return build.october_2006
                   ? "NW4R SND of 2006, with AX of October 2006, from the game's main.dol"
                   : "NW4R SND without a banner, from the game's main.dol. It plays as the build of 2007.";
    }

    const uint32_t month = build.date / 100 % 100;
    return "NW4R SND of " + std::to_string(build.date % 100) + " " +
           (month >= 1 && month <= 12 ? kMonths[month - 1] : "?") + " " + std::to_string(build.date / 10000) +
           ", from the game's main.dol";
}

// The tracks' colours in Preferences and in the settings' file, in the order of App::TrackColours.
constexpr std::array<const char*, 3> kTrackColourNames = {"Rainbow", "Standard", "Custom"};

// The zones' lines in the program's settings: each line's key, and the zone's flag.
struct ZoneSetting
{
    std::string_view key;
    bool OpenViews::* shown;
};
constexpr std::array<ZoneSetting, 5> kZoneSettings = {{{"ShowInspector", &OpenViews::inspector},
                                                       {"ShowBrowser", &OpenViews::browser},
                                                       {"ShowMixer", &OpenViews::mixer},
                                                       {"ShowVariables", &OpenViews::variables},
                                                       {"ShowUndoHistory", &OpenViews::undo_history}}};

// The version of the default layout. A layout saved from an older one lacks zones, and the program rebuilds it.
constexpr int kLayoutVersion = 4;

// The browser's groups, in order. Sounds holds the archive's stream sounds and wave sounds. Sequences holds its
// sequence sounds. Waves holds its wave archives. Wave Sound Files holds its wave sound files, each with its wave
// sounds.
constexpr std::size_t kSoundsGroup = 0;
constexpr std::size_t kSequencesGroup = 1;
constexpr std::size_t kBanksGroup = 2;
constexpr std::size_t kWavesGroup = 3;
constexpr std::size_t kWaveSoundFilesGroup = 4;

// The browser's menu items that open a bank and its instruments in the Bank Editor, a wave in its properties and a
// stream sound's stream in its properties. A double-click does the same. Like Open Bank Editor, each opens a window at
// once. None takes an ellipsis.
constexpr char kOpenBankEditor[] = "Open Bank Editor";
constexpr char kWaveProperties[] = "Wave Properties";
constexpr char kStreamProperties[] = "Stream Properties";

// The version of the Wii's wave files (RWAV) that a WAV file dropped on a bank of the Wii becomes.
constexpr uint16_t kWiiWaveVersion = 0x0102;

// The sound archives that File > Import > Sound Archive shows: the Switch's and the Wii U's, the 3DS's and the Wii's.
// Nd Cube's games name theirs `.fsst` (sub-archives) and `.fspj` (the resident one).
constexpr SDL_DialogFileFilter kArchiveFilters[] = {{"Sound archives", "bfsar;bcsar;brsar;fsst;fspj"}};

// The sound archive that File > Export writes, by its console: the Switch's and the Wii U's, the 3DS's and the Wii's.
// On the Mac, a name typed without an extension takes the filter's first. The other consoles' extensions stay out of
// the filter: the export would add the console's extension after one of them.
constexpr SDL_DialogFileFilter kLaterArchiveFilters[] = {{"Sound archives", "bfsar;fsst;fspj"}};
constexpr SDL_DialogFileFilter k3dsArchiveFilters[] = {{"Sound archives", "bcsar"}};
constexpr SDL_DialogFileFilter kWiiArchiveFilters[] = {{"Sound archives", "brsar"}};

// The Hokuto files that File > Open and File > Save As show.
constexpr SDL_DialogFileFilter kHokutoFilters[] = {{"Hokuto files", "hok"}};

// The Export window's option of a song for the 3DS whose sound plays stereo waves (edit::MonoWaves), and its tooltip.
constexpr char kMonoWaves[] = "Stereo waves as mono";
constexpr char kMonoWavesTip[] =
    "Each stereo wave goes in as the mean of its two channels. It then takes one of the 3DS's 24 channels in place of "
    "two, and the 3DS drops fewer of the song's notes. The regions that play the wave play louder. The wave then "
    "keeps the power of its two channels. Hokuto chooses mono when the song holds more channels at once than the 3DS "
    "plays.";

// The MIDI files that File > Export writes, with their instruments beside them. A sound archive takes the filters of
// File > Import.
constexpr SDL_DialogFileFilter kMidiFilters[] = {{"MIDI files", "mid"}};

// The files of the browser's Export…, one filter for each extension: an item's file of the Wii, the 3DS, the Switch or
// the Wii U, an AAC stream, a WAV file, a bank's SoundFont and a sequence's MIDI file. The dialog shows the filter of
// the export's file.
constexpr SDL_DialogFileFilter kItemFilters[] = {{"Sequence files", "bfseq"}, {"Wave sound files", "bfwsd"},
                                                 {"Bank files", "bfbnk"},     {"Wave archive files", "bfwar"},
                                                 {"Wave files", "bfwav"},     {"Stream files", "bfstm"},
                                                 {"Sequence files", "bcseq"}, {"Wave sound files", "bcwsd"},
                                                 {"Bank files", "bcbnk"},     {"Wave archive files", "bcwar"},
                                                 {"Wave files", "bcwav"},     {"Stream files", "bcstm"},
                                                 {"Sequence files", "brseq"}, {"Wave sound files", "brwsd"},
                                                 {"Bank files", "brbnk"},     {"Wave archive files", "brwar"},
                                                 {"Wave files", "brwav"},     {"Stream files", "brstm"},
                                                 {"AAC files", "aac"},        {"WAV files", "wav"},
                                                 {"SoundFont files", "sf2"},  {"MIDI files", "mid"}};

// The files that File > Import and the browser's Replace… show for a file of the 3DS, the Wii U or the Switch: sequence
// files, bank files, wave archive files, wave sound files and wave files of the three. A file of another of them goes
// in converted.
constexpr SDL_DialogFileFilter kSequenceFilters[] = {{"Sequence files", "bcseq;bfseq"}};
constexpr SDL_DialogFileFilter kBankFilters[] = {{"Bank files", "bcbnk;bfbnk"}};
constexpr SDL_DialogFileFilter kWaveArchiveFilters[] = {{"Wave archive files", "bcwar;bfwar"}};
constexpr SDL_DialogFileFilter kWaveSoundFileFilters[] = {{"Wave sound files", "bcwsd;bfwsd"}};
constexpr SDL_DialogFileFilter kWaveFilters[] = {{"Wave files", "bcwav;bfwav"}};
constexpr SDL_DialogFileFilter kWavFilters[] = {{"WAV files", "wav"}};
constexpr SDL_DialogFileFilter kWaveReplaceFilters[] = {{"WAV files and wave files", "wav;bcwav;bfwav"}};
constexpr SDL_DialogFileFilter kStreamFilters[] = {{"Stream files and WAV files", "bfstm;bcstm;wav"}};
constexpr SDL_DialogFileFilter kAacStreamFilters[] = {
    {"AAC files, WAV files and stream files", "aac;wav;bcstm;bfstm;brstm"}};

// The files that a stream sound of the Wii takes: stream files of the Wii and of the later consoles, and WAV files.
constexpr SDL_DialogFileFilter kWiiStreamFilters[] = {{"Stream files and WAV files", "brstm;bfstm;bcstm;wav"}};

// The files that File > Import and the browser's Replace… show for a file of the Wii: sequence files, bank files, wave
// archive files, wave sound files and wave files of the Wii, and those of the 3DS, the Wii U and the Switch. The later
// consoles' files go in converted.
constexpr SDL_DialogFileFilter kWiiSequenceFilters[] = {{"Sequence files", "brseq;bcseq;bfseq"}};
constexpr SDL_DialogFileFilter kWiiBankFilters[] = {{"Bank files", "brbnk;bcbnk;bfbnk"}};
constexpr SDL_DialogFileFilter kWiiWaveArchiveFilters[] = {{"Wave archive files", "brwar;bcwar;bfwar"}};
constexpr SDL_DialogFileFilter kWiiWaveSoundFileFilters[] = {{"Wave sound files", "brwsd;bcwsd;bfwsd"}};
constexpr SDL_DialogFileFilter kWiiWaveFilters[] = {{"Wave files", "brwav;bcwav;bfwav"}};
constexpr SDL_DialogFileFilter kWiiWaveReplaceFilters[] = {{"WAV files and wave files", "wav;brwav;bcwav;bfwav"}};

// Whether the Wii's `archive` holds a file whose wave data can take a new wave: a bank's or a wave sound's.
bool HoldsWaves(const ntdlib::rsar::SoundArchive& archive)
{
    return !archive.Banks().empty() ||
           std::ranges::any_of(archive.Sounds(), [](const ntdlib::rsar::SoundInfo& sound)
                               { return sound.type == ntdlib::rsar::SoundType::kWaveSound; });
}

// What the browser lists under bank or wave archive `index` of `archive`, by the item's group: a bank's programs, or a
// wave archive's waves. A bank or a wave archive that can't be read lists none.
std::vector<uint32_t> ChildrenOf(const ntdlib::sar::SoundArchive& archive, std::size_t group, std::size_t index)
{
    std::vector<uint32_t> children;
    try
    {
        if (group == kBanksGroup)
        {
            children = song::ProgramsOf(archive, index);
        }
        else
        {
            children.resize(song::WaveCountOf(archive, index));
            std::iota(children.begin(), children.end(), 0u);
        }
    }
    catch (const std::exception&)
    {
        children.clear();
    }

    return children;
}

// The browser's name for a child of an item of `group`: a bank's program or a wave archive's wave.
std::string ChildName(std::size_t group, uint32_t child)
{
    return (group == kBanksGroup ? "Program " : "Wave ") + std::to_string(child);
}

// The bank slots of sound `sound` of `archive`, with the programs of each slot's bank. A bank that can't be read lists
// no programs.
std::vector<SlotView> SlotsOf(const ntdlib::sar::SoundArchive& archive, uint32_t sound)
{
    std::vector<SlotView> slots;
    if (sound >= archive.Sounds().size())
    {
        return slots;
    }

    for (const uint32_t bank : archive.Sounds()[sound].banks)
    {
        SlotView& slot = slots.emplace_back();
        const uint32_t index = ntdlib::sar::ItemNumber(bank);
        if (bank == ntdlib::sar::kNoItem || index >= archive.Banks().size())
        {
            continue;
        }

        slot.bank = archive.Banks()[index].name;
        try
        {
            slot.programs = song::ProgramsOf(archive, index);
        }
        catch (const std::exception&)
        {
            slot.programs.clear();
        }
    }

    return slots;
}

// The same for sound `sound` of the Wii's `archive`. Its one slot holds the sound's bank.
std::vector<SlotView> SlotsOf(const ntdlib::rsar::SoundArchive& archive, uint32_t sound)
{
    std::vector<SlotView> slots;
    if (sound >= archive.Sounds().size())
    {
        return slots;
    }

    SlotView& slot = slots.emplace_back();
    const uint32_t bank = archive.Sounds()[sound].bank;
    if (bank >= archive.Banks().size())
    {
        return slots;
    }

    slot.bank = archive.Banks()[bank].name;
    try
    {
        slot.programs = song::ProgramsOf(archive, bank);
    }
    catch (const std::exception&)
    {
        slot.programs.clear();
    }

    return slots;
}

// The text of instrument `instrument` of a song's sound in the Undo History: its bank's name and its program.
std::string InstrumentText(const SongView& song, const edit::Program& instrument)
{
    const auto slot = static_cast<std::size_t>(instrument.slot);
    const std::string bank = slot < song.slots.size() && !song.slots[slot].bank.empty()
                                 ? song.slots[slot].bank
                                 : "Slot " + std::to_string(instrument.slot);
    return bank + " Program " + std::to_string(instrument.number);
}

// `names` joined for a sentence: "A", "A and B" or "A, B and C".
std::string JoinedNames(std::span<const std::string> names)
{
    std::string text;
    for (std::size_t i = 0; i < names.size(); i++)
    {
        text += i == 0 ? "" : i + 1 == names.size() ? " and " : ", ";
        text += names[i];
    }

    return text;
}

// The end of a sentence in a tooltip that says what `reach` changes besides a track of `read`: the sound's other
// tracks, then the other sounds of `archive`. Past four sounds, three of them stand for the rest. Empty where `reach`
// holds nothing.
std::string ReachText(std::span<const std::string> names, const song::Sequence& read, const edit::Reach& reach)
{
    std::vector<std::string> tracks;
    for (const int number : reach.tracks)
    {
        const auto found = std::find_if(read.tracks.begin(), read.tracks.end(),
                                        [number](const song::Track& track) { return track.number == number; });
        tracks.push_back(found != read.tracks.end() ? found->name : "Track " + std::to_string(number));
    }
    std::vector<std::string> sounds;
    for (const uint32_t sound : reach.sounds)
    {
        const bool named = sound < names.size() && !names[sound].empty();
        sounds.push_back(named ? names[sound] : "Sound " + std::to_string(sound));
    }

    // Such as "Track 3 of this sound, and the sounds SE_A and SE_B".
    constexpr std::size_t kMostNamed = 4;
    std::string text = tracks.empty() ? std::string() : JoinedNames(tracks) + " of this sound";
    text += !tracks.empty() && !sounds.empty() ? ", and " : "";
    if (sounds.size() == 1)
    {
        text += "the sound " + sounds.front();
    }
    else if (sounds.size() > 1 && sounds.size() <= kMostNamed)
    {
        text += "the sounds " + JoinedNames(sounds);
    }
    else if (sounds.size() > kMostNamed)
    {
        text += std::to_string(sounds.size()) + " other sounds, such as " +
                JoinedNames(std::span<const std::string>(sounds).first(kMostNamed - 1));
    }

    return text;
}

// The title of Replace…'s dialog for a file of `kind`, and the name of its action in the Undo History.
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

// The file from a dialog, with `extension` added unless its name ends in it. Every Hokuto file ends in .hok, in either
// form. An export's files end in the extension of their target.
std::string WithExtension(const std::string& path, std::string_view extension)
{
    if (xsflib::AsciiLower(xsflib::Utf8(xsflib::PathFromUtf8(path).extension())) == extension)
    {
        return path;
    }

    return path + std::string(extension);
}

// Whether `file`, the name `chosen` that a save dialog gave with the extension that WithExtension added, is another
// name, and the folder holds a file of that name. The dialog asked only about the chosen name. On the Mac, the dialog
// adds the filter's extension before it asks. The dialogs of Windows and Linux add none.
bool AddedNameExists(const std::string& chosen, const std::filesystem::path& file)
{
    std::error_code error;
    return xsflib::PathFromUtf8(chosen) != file && std::filesystem::exists(file, error);
}

// The question before an export replaces files that the folder `folder` holds: `existing` names them.
std::string ReplaceQuestion(const std::filesystem::path& folder, const std::vector<std::string>& existing)
{
    const std::string name = xsflib::Utf8(folder.filename().empty() ? folder : folder.filename());
    if (existing.size() == 1)
    {
        return "The folder " + name + " holds " + existing[0] + " already. Replace it?";
    }

    return "The folder " + name + " holds " + std::to_string(existing.size()) + " of the files already, " +
           existing[0] + " among them. Replace them?";
}

// The bytes of the stream that stream sound `sound` of `archive` plays, from `file`, the file that builds the archive.
// `archive` is an archive of the Wii or of a later console. Throws std::runtime_error with a message for the user that
// names the stream's file when the file holds no such stream.
template <class Archive>
const std::vector<uint8_t>& StreamBytes(const format::File& file, const Archive& archive, uint32_t sound)
{
    const std::optional<uint32_t> id = song::StreamFileOf(archive, sound);
    if (id && file.native)
    {
        const auto found = file.native->streams.find(*id);
        if (found != file.native->streams.end())
        {
            return *found->second.bytes;
        }
    }

    if (sound >= archive.Sounds().size())
    {
        throw std::runtime_error("The archive lacks sound " + std::to_string(sound) + ".");
    }

    throw std::runtime_error(MissingStreamText(archive.Sounds()[sound].name, song::StreamFileName(archive, sound)));
}

// Why a stream sound doesn't export: the file holds no stream for it. `name` is the stream file's name.
std::string MissingStreamNote(const std::string& name)
{
    return "The file holds no stream for this sound. Its stream, " + name +
           ", wasn't beside the archive when the archive was imported. Import the archive again with the stream "
           "beside it.";
}

// Adds the lines of `sizes` to `lines` once the thread has measured them.
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

// Whether the work that `work` waits for is done. The future of std::async blocks in its destructor until then.
template <typename T>
bool Done(const std::future<T>& work)
{
    return work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// The tags of the MIDI file of sound `sound` of `file`: the sound's title, and the sound's copyright or else the
// file's. The file's title names the whole file. Without a title of the sound, the MIDI file keeps the sound's name.
song::MidiTags MidiTagsOf(const format::File& file, uint32_t sound)
{
    const auto tag = [](const format::Tags& tags, const std::string& key)
    {
        const auto found = tags.find(key);
        return found != tags.end() ? found->second : std::string();
    };

    const auto of_sound = file.sound_tags.find(sound);
    const format::Tags sound_tags = of_sound != file.sound_tags.end() ? of_sound->second : format::Tags();
    song::MidiTags tags{tag(sound_tags, "title"), tag(sound_tags, "copyright")};
    if (tags.copyright.empty())
    {
        tags.copyright = tag(file.tags, "copyright");
    }

    return tags;
}

// Logs what failed with SDL3's reason, and shows both in a message box. A program started from Finder or Explorer has
// no console.
void ShowError(const char* what)
{
    const std::string message = std::string(what) + ": " + SDL_GetError();
    SDL_Log("%s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Hokuto", message.c_str(), nullptr);
}

// Whether `keys` type into a text field. Chords with Ctrl or Command don't, and neither do the function keys.
bool TypesText(ImGuiKeyChord keys)
{
    const auto key = static_cast<ImGuiKey>(keys & ~ImGuiMod_Mask_);
    return (keys & (ImGuiMod_Ctrl | ImGuiMod_Super)) == 0 && (key < ImGuiKey_F1 || key > ImGuiKey_F24);
}

// Whether the user holds a key or a mouse button. Dear ImGui counts the mouse buttons among its keys. Dear ImGui times
// the repeats of a held key by a clock that moves on only in frames.
bool Holding()
{
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
    {
        if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key)))
        {
            return true;
        }
    }

    return false;
}

// Moves a stereo meter on by a frame. The meter jumps up to the frame's peaks, amplitudes where 1 is full scale.
// Otherwise it falls back by `fall` dB. `peak` keeps the highest level that the meter reaches.
void FollowPeaks(const std::array<float, 2>& peaks, float fall, std::array<float, 2>& meter, float& peak)
{
    for (std::size_t i = 0; i < peaks.size(); i++)
    {
        const float level = std::max(peaks[i] > 0.0f ? 20.0f * std::log10(peaks[i]) : kSilence, meter[i] - fall);
        meter[i] = level < kMeterFloor ? kSilence : level;
    }
    peak = std::max({peak, meter[0], meter[1]});
}

// Whether a stereo meter shows a level.
bool Shows(const std::array<float, 2>& meter)
{
    return meter[0] > kSilence || meter[1] > kSilence;
}

// The auditions' lead for a device of `rate` that takes `frames` at a time.
std::chrono::milliseconds AuditionLead(int rate, int frames)
{
    if (rate <= 0 || frames <= 0)
    {
        return kLeastAuditionLead;
    }

    const int64_t lead = (2 * static_cast<int64_t>(frames) * 1000 + rate - 1) / rate;
    return std::max(std::chrono::milliseconds(lead), kLeastAuditionLead);
}

// An aux bus's volume, a gain, in the decibels that an FX channel's fader shows, and back. A gain of 0 is silence.
float DecibelsOfGain(float gain)
{
    return gain > 0.0f ? 20.0f * std::log10(gain) : kSilence;
}

float GainOfDecibels(float level)
{
    return level > kSilence ? std::min(std::pow(10.0f, level / 20.0f), format::kMaxAuxVolume) : 0.0f;
}

// The part that the arrangement shows as selected, or nullptr.
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

// The starts and ends of the selected track's parts, in quarter notes.
std::vector<double> PartBounds(const std::optional<SongView>& song, const ArrangementState& state)
{
    std::vector<double> bounds;
    if (!song || state.selected_track < 0 || state.selected_track >= static_cast<int>(song->tracks.size()))
    {
        return bounds;
    }

    const double quarter = song->ticks_per_quarter;
    for (const PartView& part : song->tracks[static_cast<std::size_t>(state.selected_track)].parts)
    {
        bounds.push_back(part.start / quarter);
        bounds.push_back((part.start + part.length) / quarter);
    }

    return bounds;
}

// Browser items without children, one for each of `names`.
std::vector<BrowserItem> ItemsNamed(std::initializer_list<std::string> names)
{
    std::vector<BrowserItem> items;
    for (const std::string& name : names)
    {
        BrowserItem item;
        item.name = name;
        items.push_back(std::move(item));
    }

    return items;
}

// Calls `visit` with each of `items` and each item of their submenus.
template <typename Visit>
void VisitItems(const std::vector<MenuItem>& items, const Visit& visit)
{
    for (const MenuItem& item : items)
    {
        visit(item);
        VisitItems(item.items, visit);
    }
}

// Whether versions `a` and `b` of a file build the same archive. The versions share the sequences and the native
// archive that their edits leave alone.
bool SameArchive(const format::File& a, const format::File& b)
{
    const auto shared = [](const auto& x, const auto& y)
    {
        return x.SharesWith(y);
    };
    return a.native.SharesWith(b.native) &&
           std::equal(a.sequences.begin(), a.sequences.end(), b.sequences.begin(), b.sequences.end(), shared);
}

// The mixing mode in which sound `sound` of `file` plays: the file's mode for the sound, or strict mixing on an engine
// that takes no other (format::MixingRefused), such as the Wii's. The mode that the file keeps for the sound stays.
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

// Whether `archive` holds a sound of one of `types`.
template <class Archive, class Type>
bool HoldsSoundOf(const Archive& archive, std::initializer_list<Type> types)
{
    return std::ranges::any_of(
        archive.Sounds(), [types](const auto& sound) { return std::ranges::find(types, sound.type) != types.end(); });
}

// Whether sound `sound` of `file` plays its conversion for another console's engine (edit::ForEngine).
bool Converts(const format::File& file, uint32_t sound)
{
    const format::Engine engine = format::EngineOf(file, sound);
    return engine != file.engine && !edit::EngineRefused(file, engine);
}

// The semitones that the last transpose of `controls` before `tick` sets, from a track's commands (ControlsOf), or 0. A
// transpose from a random number or a variable counts as 0.
int TransposeAt(std::span<const KeyControl> controls, uint32_t tick)
{
    int semitones = 0;
    for (const KeyControl& control : controls)
    {
        const ntdlib::commands::Argument& value = control.command.value;
        if (control.command.kind == ntdlib::commands::Kind::kTranspose && control.tick <= tick)
        {
            const bool decided = value.source == ntdlib::commands::Source::kValue;
            semitones = decided ? static_cast<int>(value.value / ntdlib::commands::kOne / 100) : 0;
        }
    }

    return semitones;
}

// The note to add for `drawn`, a note that the Key Editor drew on `track`, in sequence `sequence` of `file`. The note
// goes into the last stretch of the track's commands that started at or before its tick. Its key leaves out the
// transpose of the note before it, or else of the track's first note. A track without notes takes the transpose of
// `controls`, the commands of its settings (ControlsOf). Throws std::invalid_argument for a tick before the track's
// first stretch.
edit::NewNote NewNoteOf(const format::File& file, uint32_t sequence, const song::Track& track,
                        std::span<const KeyControl> controls, const KeyNote& drawn)
{
    const std::optional<song::Stretch> stretch = edit::StretchAt(track.stretches, drawn.start);
    if (!stretch)
    {
        throw std::invalid_argument("A new note can't go at that tick on this track.");
    }
    if (track.notes.empty())
    {
        return {*stretch, drawn.start, drawn.key - TransposeAt(controls, drawn.start), drawn.velocity, drawn.length};
    }

    // The track's notes come in the order that they start.
    const song::Note* before = &track.notes.front();
    for (const song::Note& note : track.notes)
    {
        if (note.start > drawn.start)
        {
            break;
        }

        before = &note;
    }

    const int transpose = before->key - edit::CommandKey(file, sequence, before->command);
    return {*stretch, drawn.start, drawn.key - transpose, drawn.velocity, drawn.length};
}

// The names in the Undo History of a move of `track`'s control from `from` to `to`, in the values of its setting. Each
// name gives a channel or a send slot, then the parameter, such as Track 0 / Volume or 1- FX A / Level. The name of a
// pan gives the track. The details give the values as the mixer shows them. The name of a send slot leaves the track
// out, and a send's details name it.
ActionNames MixMoveNames(const std::string& track, MixControl control, int from, int to)
{
    const auto text = [control](int value)
    {
        const float shown = ShownValue(control, value);
        return control == MixControl::kPan ? PanText(shown) : LevelText(shown) + " dB";
    };
    const std::string change = "from " + text(from) + " to " + text(to);
    switch (control)
    {
    case MixControl::kVolume:
        return {track + " / Volume", change};
    case MixControl::kPan:
        return {track + " / Pan", change};
    default:
        break;
    }

    const int send = static_cast<int>(control) - static_cast<int>(MixControl::kSendA);
    const std::string slot = std::to_string(send + 1) + "- FX " + static_cast<char>('A' + send);
    return {slot + " / Level", "Track: " + track + " - " + change};
}

// The names of `change` of the values of `view`, a sound in the tables of the version before the change. Change Sound
// Volume tells the edit apart from a change of a track's volume in the mixer. A switch of the tracks turns one track on
// or off.
ActionNames SoundChangeNames(const SoundView& view, const edit::SoundChange& change)
{
    const uint32_t from = edit::ValueOf(view.values, change.value, change.slot);
    const std::string where = "Sound: " + view.values.name + " - ";
    const std::string from_to =
        "from " + SoundValueText(view, change.value, from) + " to " + SoundValueText(view, change.value, change.to);
    switch (change.value)
    {
    case edit::SoundValue::kVolume:
        return {"Change Sound Volume", where + from_to};

    case edit::SoundValue::kBank:
        return {"Change Bank Slot", where + "Slot " + std::to_string(change.slot) + " " + from_to};

    case edit::SoundValue::kTracks:
        {
            const uint32_t turned = from ^ change.to;
            if (std::has_single_bit(turned))
            {
                const int track = std::countr_zero(turned);
                return {"Change Tracks",
                        where + "Track " + std::to_string(track) + ((change.to & turned) != 0 ? " on" : " off")};
            }
            break;
        }

    default:
        break;
    }

    return {"Change " + edit::NameOf(change.value), where + from_to};
}

// A scene's values as the starting values of its sound's variables. The starting values leave out a player variable's
// -1, the value that it has without a choice. A global variable keeps -1: on the Wii, it starts at 0 without a choice.
song::StartValues SceneStartValues(const format::Scene& scene)
{
    song::StartValues values;
    for (const auto& [variable, value] : scene.values)
    {
        if (value != -1 || variable >= song::kFirstGlobal)
        {
            values[variable] = value;
        }
    }

    return values;
}

// The starting values of a sound's variables as a scene's values. A scene holds the variables that a game sets only.
std::map<uint8_t, int16_t> SceneValues(const song::StartValues& values)
{
    std::map<uint8_t, int16_t> scene;
    for (const auto& [variable, value] : values)
    {
        if (variable >= 0 && variable < format::kSceneVariables)
        {
            scene[static_cast<uint8_t>(variable)] = value;
        }
    }

    return scene;
}

// A scene's changes as the changes of its sound's variables, and the other way round.
song::Changes SceneChanges(const format::Scene& scene)
{
    song::Changes changes;
    for (const format::VariableChange& change : scene.changes)
    {
        changes.push_back({change.time, change.variable, change.value});
    }

    return changes;
}

std::vector<format::VariableChange> FileChanges(const song::Changes& changes)
{
    std::vector<format::VariableChange> scene;
    for (const song::VariableChange& change : changes)
    {
        if (change.variable >= 0 && change.variable < format::kSceneVariables)
        {
            scene.push_back({change.time, static_cast<uint8_t>(change.variable), change.value});
        }
    }

    return scene;
}

// The first of Scene 1, Scene 2 and so on that no scene of sound `sound` among `scenes` has.
std::string NewSceneName(const std::vector<format::Scene>& scenes, uint32_t sound)
{
    for (int number = 1;; number++)
    {
        const std::string name = "Scene " + std::to_string(number);
        const auto taken = [sound, &name](const format::Scene& scene)
        {
            return scene.sound == sound && scene.name == name;
        };
        if (std::none_of(scenes.begin(), scenes.end(), taken))
        {
            return name;
        }
    }
}

// The field of `document`'s native layout that holds each sound's start (format::StartFieldsOfSounds), or nothing when
// the archive's tables can't be read. `bytes` are the archive that the layout builds. Without the fields, Replace…
// stays disabled.
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

// `selection`, a selection of the commands of a sequence of `edited`, in the version that settles the timelines of
// `edited` (edit::SettleTimelines). Each selected command keeps its place in its settled sequence.
std::optional<KeySelection> SettledSelection(std::optional<KeySelection> selection, const format::File& edited,
                                             const edit::Settled& settled)
{
    if (!selection || !edited.native)
    {
        return selection;
    }

    for (const auto& [sequence, items] : settled.items)
    {
        if (sequence >= edited.sequences.size() || !edited.sequences[sequence].SharesWith(selection->sequence))
        {
            continue;
        }

        const song::Console console = edit::ConsoleOf(edited);
        const std::vector<uint32_t> from = edit::Offsets(*edited.sequences[sequence], console);
        const std::vector<uint32_t> to = edit::Offsets(*settled.file.sequences[sequence], console);
        std::set<uint32_t> commands;
        for (const uint32_t offset : selection->commands)
        {
            const std::optional<uint32_t> item = edit::ItemAtOffset(from, offset);
            if (item && *item < items.size() && items[*item])
            {
                commands.insert(to[*items[*item]]);
            }
        }
        selection->sequence = settled.file.sequences[sequence];
        selection->commands = std::move(commands);
    }

    return selection;
}

// `name` without its extension: the part from its last dot on. A name without a dot stays as it is.
std::string WithoutExtension(const std::string& name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// The part of the name of an instrument's files after its bank's name: its program, padded to the digits of the bank's
// last program with an instrument, `last`. The bank's SFZ files take the same names.
std::string ProgramPart(uint32_t program, uint32_t last)
{
    const std::string digits = std::to_string(program);
    const std::size_t width = std::to_string(last).size();
    return "_" + std::string(digits.size() < width ? width - digits.size() : 0, '0') + digits;
}

// Gives the SFZ file of an instrument that goes out by itself, at `program`, the name of its folder, `folder`. The
// converter pads a program to the digits of the bank's last program.
void NameInstrumentSfz(ntdlib::sequence_export::BankConversion& conversion, const std::optional<uint32_t>& program,
                       const std::string& folder)
{
    if (program && conversion.sfz.files.size() == 1)
    {
        conversion.sfz.files.front().name = folder + ".sfz";
    }
}

// `bank` with only its instrument at `program`, at that program.
ntdlib::sar::BankFile InstrumentAlone(ntdlib::sar::BankFile bank, uint32_t program)
{
    for (uint32_t other = 0; other < bank.instruments.size(); other++)
    {
        if (other != program && bank.instruments[other])
        {
            bank = edit::WithoutInstrument(std::move(bank), other);
        }
    }

    return bank;
}

// Whether the file at `path` starts as a file that File > Open takes: a Hokuto file in either form, or a sound archive
// of the Wii, the 3DS, the Wii U or the Switch. A file that can't be read counts as one: the open then says why.
bool OpensAsDocument(const std::string& path)
{
    std::ifstream file(xsflib::PathFromUtf8(path), std::ios::binary);
    if (!file)
    {
        return true;
    }

    std::vector<uint8_t> head(64);
    file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(file.gcount()));

    const auto starts = [&head](std::string_view magic)
    {
        return head.size() >= magic.size() && std::equal(magic.begin(), magic.end(), head.begin());
    };
    return format::IsBinary(head) || format::IsText(head) || starts("RSAR") || starts("CSAR") || starts("FSAR");
}

// Dear ImGui's back end gives SDL a text input area 1 pixel wide at the caret. On Windows, SDL makes that area the
// IME's composition window, and the composition then has no room to show. The area runs from the caret to the
// window's right edge instead.
void (*g_backend_set_ime_data)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*) = nullptr;

void SetImeData(ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
{
    g_backend_set_ime_data(context, viewport, data);
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (window == nullptr || !data->WantVisible)
        return;
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    SDL_Rect area;
    area.x = static_cast<int>(data->InputPos.x - viewport->Pos.x);
    area.y = static_cast<int>(data->InputPos.y - viewport->Pos.y);
    area.w = std::max(1, width - area.x);
    area.h = static_cast<int>(data->InputLineHeight);
    SDL_SetTextInputArea(window, &area, 0);
}

} // namespace

std::unique_ptr<App> App::Create(bool preview, const std::string& file)
{
    std::unique_ptr<App> app(new App(preview));
    if (!app->Init())
    {
        return nullptr;
    }

    if (!file.empty())
    {
        app->OpenOrImport(file);
    }

    return app;
}

App::App(bool preview)
    : ring_(kRingFrames),
      worker_(ring_, kAhead, kBlockFrames),
      audition_ring_(kRingFrames),
      audition_worker_(audition_ring_, kLeastAuditionLead, kAuditionBlockFrames),
      output_(ring_, audition_ring_, kDefaultRate),
      file_name_(preview ? "Preview" : kUntitled),
      browser_{{"Sounds", Icon::kSound, theme::kSky, {}},
               {"Sequences", Icon::kSequence, theme::kAccent, {}},
               {"Banks", Icon::kBank, theme::kCycle, {}},
               {"Waves", Icon::kWave, theme::kPlay, {}},
               {"Wave Sound Files", Icon::kSound, theme::kSky, {}}},
      preview_(preview)
{
    if (!preview)
    {
        return;
    }

    // The preview's song. The browser lists names like those in a game's sound archive.
    song_ = MakePreviewSong();
    transport_.SetTempo(song_->tempo);
    transport_.SetBeatsPerBar(song_->beats_per_bar);
    transport_.SetLocators(32.0, 64.0);
    browser_[0].items = ItemsNamed({"SE_CURSOR", "SE_DECIDE", "SE_CANCEL", "SE_JUMP"});
    browser_[1].items = ItemsNamed({"SEQ_TITLE", song_->name, "SEQ_FIELD", "SEQ_BATTLE", "SEQ_ENDING"});
    browser_[2].items = ItemsNamed({"BANK_FIELD", "BANK_BATTLE", "BANK_SE"});
    browser_[2].items[0].children = {"Program 0", "Program 1", "Program 5"};
    browser_[3].items = ItemsNamed({"WAVE_BRASS", "WAVE_STRINGS", "WAVE_DRUMS"});
    browser_state_.selected = {1, 1, -1};
    browser_state_.open.insert({2, 0});

    // The trumpet's first part is selected. The inspector then shows a part's details.
    arrangement_.selected_part_track = 0;
    arrangement_.selected_part = 0;

    // The song has three scenes. Its variables start with the first one's values. FX A holds a reverb. The history of
    // the made-up song's effects shows three edits a minute apart, with the last one undone.
    format::File file;
    file.scenes = {
        {0, "Calm", {{0, 1}}, {}}, {0, "Storm", {{0, 2}}, {}}, {0, "Last lap", {{0, 1}, {16, 64}}, {{45'000, 0, 2}}}};
    start_values_[0] = SceneStartValues(file.scenes[0]);
    changes_[0] = SceneChanges(file.scenes[0]);
    history_.emplace(std::make_shared<const format::File>(file));
    const EffectPlace place{0, 0};
    const auto now = std::chrono::system_clock::now();
    const auto add = [this, &file, now](const ActionNames& names, int minutes_ago)
    {
        const auto time = now - std::chrono::minutes(minutes_ago);
        history_rows_.push_back({names.name, names.details, TimeOfDay(time)});
        history_->Do({names.name, names.details, time, std::make_shared<const format::File>(file)});
    };

    file.aux_effects[0].push_back(NewEffect(file, file.engine, 0));
    add({"Add Effect", EffectTitle(place, file.aux_effects[0][0])}, 3);
    for (const auto& [parameter, value] : {std::pair<std::size_t, float>{2, 2.5f}, {5, 0.03f}})
    {
        format::Effect changed = file.aux_effects[0][0];
        changed.parameters[parameter] = value;
        const ActionNames names = EffectChangeNames(place, EffectEdit::kChanged, file.aux_effects[0][0], changed);
        file.aux_effects[0][0] = changed;
        add(names, parameter == 2 ? 2 : 1);
    }
    history_->Undo();
}

App::~App()
{
    // The device stops taking frames before anything else closes. An export's measure stops too. Without that, the
    // destruction of its future would wait for the whole render.
    output_.Close();
    DropExport();

    if (gpu_ != nullptr)
    {
        SDL_WaitForGPUIdle(gpu_);
    }
    if (imgui_sdl3_)
    {
        ImGui_ImplSDL3_Shutdown();
    }
    if (imgui_gpu_)
    {
        ImGui_ImplSDLGPU3_Shutdown();
    }

    // Destroying the context saves the layout and the settings.
    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::DestroyContext();
    }

    if (window_claimed_)
    {
        SDL_ReleaseWindowFromGPUDevice(gpu_, window_);
    }
    if (gpu_ != nullptr)
    {
        SDL_DestroyGPUDevice(gpu_);
    }
    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
    }
}

bool App::Init()
{
    // The window's close button quits through its event alone. SDL3 would send SDL_EVENT_QUIT after it, and the
    // question about the document's changes would come twice.
    SDL_SetAppMetadata("Hokuto", HOKUTO_VERSION, HOKUTO_APP_ID);
    SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");

    // The display sleeps when the system's settings say. SDL3 keeps it awake by default, for games and players. A
    // display left on can burn in, and nothing in the program requires it.
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        ShowError("SDL_Init failed");
        return false;
    }

    // Dear ImGui's layout and the program's settings share one file in the user's settings folder. Without the folder,
    // nothing is saved. In a DAW, the keyboard drives commands, not the focus. Floating windows move by their title
    // bars.
    char* folder = SDL_GetPrefPath("", "Hokuto");
    if (folder != nullptr)
    {
        settings_path_ = std::string(folder) + "hokuto.ini";
        SDL_free(folder);
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = settings_path_.empty() ? nullptr : settings_path_.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDpiScaleFonts = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    AddSettingsHandler();
    if (io.IniFilename != nullptr)
    {
        ImGui::LoadIniSettingsFromDisk(io.IniFilename);
    }
    if (settings_.layout_version != kLayoutVersion)
    {
        settings_.layout_version = kLayoutVersion;
        reset_layout_ = true;
        views_ = OpenViews();
    }
    saved_views_ = views_;

    // The main window opens maximised. Restored, the window takes its saved size, or a default size for the display.
    // SDL3 maximises the window when it's shown.
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    if (settings_.window_width <= 0 || settings_.window_height <= 0)
    {
        settings_.window_width = static_cast<int>(kDefaultWidth * scale);
        settings_.window_height = static_cast<int>(kDefaultHeight * scale);
    }
    SDL_WindowFlags flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED;
    const std::string title = "Hokuto - " + file_name_;
    window_ = SDL_CreateWindow(title.c_str(), settings_.window_width, settings_.window_height, flags);
    if (window_ == nullptr)
    {
        ShowError("SDL_CreateWindow failed");
        return false;
    }

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowMinimumSize(window_, static_cast<int>(960.0f * scale), static_cast<int>(600.0f * scale));
    SetTitleBarColour(window_, theme::kRaised);
    SDL_ShowWindow(window_);
    native_menus_ = InstallMenuBar(Menus(), *this);

    // The GPU: Metal on the Mac, Direct3D 12 or Vulkan on Windows.
    gpu_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
                                   SDL_GPU_SHADERFORMAT_METALLIB,
                               kGpuDebug, nullptr);
    if (gpu_ == nullptr)
    {
        ShowError("SDL_CreateGPUDevice failed");
        return false;
    }

    window_claimed_ = SDL_ClaimWindowForGPUDevice(gpu_, window_);
    if (!window_claimed_)
    {
        ShowError("SDL_ClaimWindowForGPUDevice failed");
        return false;
    }

    SDL_SetGPUSwapchainParameters(gpu_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    // The program's look, and Dear ImGui's back ends for SDL3 and SDL_GPU.
    theme::Apply(ImGui::GetStyle(), scale);
    imgui_sdl3_ = ImGui_ImplSDL3_InitForSDLGPU(window_);
    if (imgui_sdl3_)
    {
        g_backend_set_ime_data = ImGui::GetPlatformIO().Platform_SetImeDataFn;
        ImGui::GetPlatformIO().Platform_SetImeDataFn = SetImeData;
    }
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = gpu_;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_, window_);
    imgui_gpu_ = imgui_sdl3_ && ImGui_ImplSDLGPU3_Init(&info);
    if (!imgui_gpu_)
    {
        ShowError("Dear ImGui's back ends failed to start");
        return false;
    }

    LoadFonts();

    // The program runs without an audio device. Studio Setup says why there's none. The device rests after kOutputRest
    // without sound (RestOutput).
    if (!output_.Open(settings_.device))
    {
        SDL_Log("No audio device: %s", output_.Error().c_str());
    }
    output_sounded_ = SDL_GetTicksNS();

    // A quit signal ends a wait for events. The first frames draw at the display's rate. The program counts its start
    // as an event.
    wake_event_ = SDL_RegisterEvents(1);
    WakeOnQuitSignals(wake_event_);
    last_event_ = SDL_GetTicksNS();

    return true;
}

void App::LoadFonts()
{
    // The system's fonts. Dear ImGui's font draws the text on a system without them, and the text font takes the place
    // of a font that the system lacks.
    ImGuiIO& io = ImGui::GetIO();
    theme::Fonts fonts = AddSystemFonts(*io.Fonts);
    if (fonts.text == nullptr)
    {
        fonts.text = io.Fonts->AddFontDefault();
    }
    for (ImFont** font : {&fonts.strong, &fonts.caps, &fonts.figures})
    {
        *font = *font != nullptr ? *font : fonts.text;
    }
    io.FontDefault = fonts.text;
    theme::SetFonts(fonts);
}

void App::AddSettingsHandler()
{
    // The file holds one entry for the program, [Hokuto][Settings], with one setting a line.
    ImGuiSettingsHandler handler;
    handler.TypeName = "Hokuto";
    handler.TypeHash = ImHashStr("Hokuto");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* self, const char* name) -> void*
    {
        return std::strcmp(name, "Settings") == 0 ? self->UserData : nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line)
    {
        static_cast<App*>(entry)->ReadSetting(line);
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* out)
    {
        static_cast<const App*>(self->UserData)->WriteSettings(*out);
    };
    ImGui::AddSettingsHandler(&handler);
}

void App::ReadSetting(std::string_view line)
{
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos)
    {
        return;
    }

    const std::string_view key = line.substr(0, equals);
    const std::string_view value = line.substr(equals + 1);
    int number = 0;
    const bool is_number = std::from_chars(value.data(), value.data() + value.size(), number).ec == std::errc();
    for (const ZoneSetting& zone : kZoneSettings)
    {
        if (key == zone.key && is_number)
        {
            views_.*zone.shown = number != 0;
        }
    }
    if (key == "AudioDevice")
    {
        settings_.device = value;
    }
    else if (key == "WindowWidth" && is_number)
    {
        settings_.window_width = number;
    }
    else if (key == "WindowHeight" && is_number)
    {
        settings_.window_height = number;
    }
    else if (key == "LayoutVersion" && is_number)
    {
        settings_.layout_version = number;
    }
    else if (key == "TrackListWidth" && is_number)
    {
        arrangement_.track_list_width = std::clamp(static_cast<float>(number), kMinTrackListWidth, kMaxTrackListWidth);
    }
    else if (key == "TempoTrack" && is_number)
    {
        arrangement_.tempo_row = number != 0;
    }
    else if (key == "RecentProject" && settings_.recent_projects.size() < kMostRecentProjects &&
             std::ranges::find(settings_.recent_projects, value) == settings_.recent_projects.end())
    {
        settings_.recent_projects.emplace_back(value);
    }
    else if (key == "TrackColours")
    {
        for (std::size_t i = 0; i < kTrackColourNames.size(); i++)
        {
            if (value == kTrackColourNames[i])
            {
                settings_.track_colours = static_cast<TrackColours>(i);
            }
        }
    }
}

void App::WriteSettings(ImGuiTextBuffer& out) const
{
    out.appendf("[Hokuto][Settings]\n");
    out.appendf("AudioDevice=%s\n", settings_.device.c_str());
    out.appendf("WindowWidth=%d\n", settings_.window_width);
    out.appendf("WindowHeight=%d\n", settings_.window_height);
    out.appendf("LayoutVersion=%d\n", settings_.layout_version);
    out.appendf("TrackListWidth=%d\n", static_cast<int>(std::lround(arrangement_.track_list_width)));
    out.appendf("TempoTrack=%d\n", arrangement_.tempo_row ? 1 : 0);
    for (const ZoneSetting& zone : kZoneSettings)
    {
        out.appendf("%.*s=%d\n", static_cast<int>(zone.key.size()), zone.key.data(), views_.*zone.shown ? 1 : 0);
    }
    for (const std::string& path : settings_.recent_projects)
    {
        out.appendf("RecentProject=%s\n", path.c_str());
    }
    out.appendf("TrackColours=%s\n\n", kTrackColourNames[static_cast<std::size_t>(settings_.track_colours)]);
}

SDL_AppResult App::HandleEvent(const SDL_Event& event)
{
    // Each event, a wake included, makes the window draw on for a moment (see Pace).
    last_event_ = SDL_GetTicksNS();
    ImGui_ImplSDL3_ProcessEvent(&event);

    // A quit goes through the command Quit. A text field that's being typed in keeps its text. The user then decides
    // about the document's changes. The quit of macOS's menus, the Dock and a logout come as SDL_EVENT_QUIT. The
    // window's close button sends no quit after its event (Init).
    switch (event.type)
    {
    case SDL_EVENT_QUIT:
        Execute(Command::kQuit);
        break;

    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == SDL_GetWindowID(window_))
        {
            Execute(Command::kQuit);
        }
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        // The size of a maximized window isn't the one to save.
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) == 0)
        {
            settings_.window_width = event.window.data1;
            settings_.window_height = event.window.data2;
        }
        break;

    case SDL_EVENT_AUDIO_DEVICE_ADDED:
    case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        if (!event.adevice.recording)
        {
            output_.DevicesChanged();
        }
        break;

    // Files dropped on the Bank Editor wait for the drop to complete. Their waves then make regions or instruments
    // where the first of them landed. Elsewhere, a Hokuto file dropped on the window opens, and a sound archive is
    // imported. So does a file that Finder opens with the program.
    case SDL_EVENT_DROP_BEGIN:
        bank_drops_.clear();
        bank_drop_.reset();
        window_drops_.clear();
        break;

    case SDL_EVENT_DROP_FILE:
        if (event.drop.data == nullptr)
        {
            break;
        }

        if (bank_editor_ && event.drop.windowID == SDL_GetWindowID(window_) &&
            OverBankEditor(bank_state_, ImVec2(event.drop.x, event.drop.y)))
        {
            if (bank_drops_.empty())
            {
                bank_drop_ = DropAt(bank_state_, ImVec2(event.drop.x, event.drop.y));
            }
            bank_drops_.emplace_back(event.drop.data);
        }
        else
        {
            window_drops_.emplace_back(event.drop.data);
        }
        break;

    case SDL_EVENT_DROP_COMPLETE:
        if (!bank_drops_.empty() && bank_drop_)
        {
            DropFilesOnBank(bank_drops_, *bank_drop_);
        }
        else if (!bank_drops_.empty())
        {
            ShowMessage("Drop WAV files or wave files on the map or on the list of programs.", MessageKind::kNote);
        }
        bank_drops_.clear();
        bank_drop_.reset();
        if (!window_drops_.empty())
        {
            DropFilesOnWindow(std::exchange(window_drops_, {}));
        }
        break;

    default:
        break;
    }

    // A minimized window draws no frames. The quit can't wait for one.
    return quit_ ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

SDL_AppResult App::Iterate()
{
    // A minimized window draws nothing. It waits for the event that restores it.
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        SetWaiting(true);
        RestOutput();
        return SDL_APP_CONTINUE;
    }

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (const char* window = std::exchange(focus_next_frame_, nullptr))
    {
        ImGui::SetWindowFocus(window);
    }
    HandleShortcuts();

    // A file from a dialog opens, or the file is saved to it, before the frame is drawn.
    // A file chosen for File > Import, other than a sound archive, waits while the program builds the archive of an
    // edit. The import reads the archive of the version that the history has reached.
    std::optional<std::pair<Dialog, std::string>> chosen;
    {
        std::lock_guard lock(chosen_mutex_);
        constexpr std::array kImports = {Dialog::kImportSequence,      Dialog::kImportBank, Dialog::kImportWaveArchive,
                                         Dialog::kImportWaveSoundFile, Dialog::kImportWave, Dialog::kImportAudioFile,
                                         Dialog::kImportStream};
        if (!chosen_ || std::ranges::find(kImports, chosen_->first) == kImports.end() || reading_version_ == nullptr)
        {
            chosen = std::exchange(chosen_, std::nullopt);
        }
    }
    if (chosen && chosen->first == Dialog::kSaveAs)
    {
        // What waited for the save follows it. A cancelled dialog gives no file. It drops what waited, and so does a
        // save that fails.
        const std::function<void()> then = std::exchange(after_save_, nullptr);
        const std::string path = chosen->second.empty() ? std::string() : WithExtension(chosen->second, ".hok");
        if (!path.empty() && MayWriteTo(chosen->second, xsflib::PathFromUtf8(path)))
        {
            SaveTo(path);
            if (then && history_ && !history_->Modified())
            {
                then();
            }
        }
    }
    else if (chosen && (chosen->first == Dialog::kExport || chosen->first == Dialog::kExportFolder))
    {
        ExportTo(chosen->second);
    }
    else if (chosen && chosen->first == Dialog::kReplace)
    {
        OpenReplacement(chosen->second);
    }
    else if (chosen && chosen->first == Dialog::kImportSequence)
    {
        OpenSequenceImport(chosen->second);
    }
    else if (chosen && (chosen->first == Dialog::kImportBank || chosen->first == Dialog::kImportWaveArchive ||
                        chosen->first == Dialog::kImportWaveSoundFile))
    {
        const edit::FileKind kind = chosen->first == Dialog::kImportBank          ? edit::FileKind::kBank
                                    : chosen->first == Dialog::kImportWaveArchive ? edit::FileKind::kWaveArchive
                                                                                  : edit::FileKind::kWaveSoundFile;
        OpenItemImport(chosen->second, kind);
    }
    else if (chosen && (chosen->first == Dialog::kImportWave || chosen->first == Dialog::kImportAudioFile))
    {
        OpenWaveImport(chosen->second, chosen->first == Dialog::kImportAudioFile);
    }
    else if (chosen && chosen->first == Dialog::kImportStream)
    {
        OpenStreamImport(chosen->second);
    }
    else if (chosen)
    {
        OpenOrImport(chosen->second, chosen->first == Dialog::kImport);
    }

    // A file whose open has finished shows before the frame is drawn.
    FinishOpening();

    // A measure of the groups that a view dropped goes once it's done. Until then, its lambda holds a version and the
    // version's archive.
    std::erase_if(dropped_group_sizes_, Done<std::vector<std::string>>);

    // A sequence that the reader has read opens before the frame is drawn too. A read that built the archive of a
    // version brings the archive. A new read of the sequence that the zones show follows an edit.
    if (std::optional<song::SequenceReader::Result> read = reader_.Take())
    {
        if (read->sequence && (read->archive != archive_ || read->wii != wii_archive_))
        {
            archive_ = read->archive;
            wii_archive_ = read->wii;
            archive_version_ = reading_version_;
            FollowItems();
        }
        reading_version_.reset();

        // A timeline shows the first pass of its loop, on the Wii too. The performer can find the loop a whole number
        // of passes late in a conversion for another engine as well. Playback there ends at the conversion's loop.
        const std::optional<uint32_t> sound_file =
            !read->sequence ? std::nullopt
            : archive_ && read->sequence->sound < archive_->Sounds().size()
                ? std::optional(archive_->Sounds()[read->sequence->sound].file_id)
            : wii_archive_ && read->sequence->sound < wii_archive_->Sounds().size()
                ? std::optional(wii_archive_->Sounds()[read->sequence->sound].file_id)
                : std::nullopt;
        if (sound_file && archive_version_)
        {
            const uint32_t file = *sound_file;
            const std::optional<uint32_t> sequence =
                file < file_sequences_.size() ? file_sequences_[file] : std::nullopt;
            if (sequence && edit::IsTimeline(*archive_version_, *sequence))
            {
                read->sequence = std::make_shared<const song::Sequence>(
                    edit::TimelineRead(*archive_version_, *sequence, *read->sequence));
                if (read->engine_sequence)
                {
                    read->engine_sequence = std::make_shared<const song::Sequence>(
                        edit::TimelineRead(*archive_version_, *sequence, *read->engine_sequence));
                }
            }
        }
        if (read->sequence)
        {
            TakeEngine(*read);
        }
        if (read->sequence && sequence_ && read->sequence->sound == sequence_->sound)
        {
            FollowSequence(std::move(*read));
        }
        else
        {
            ShowSequence(std::move(*read));
        }
    }

    // The transport, and the mixer's values and the tempo at its position.
    UpdatePlayback();
    if (sequence_ && song_)
    {
        const double ticks = transport_.Position() * sequence_->timebase;
        ShowMixerAt(*song_, *sequence_, ticks);
        transport_.SetTempo(song::TempoAt(*sequence_, static_cast<uint32_t>(std::max(ticks, 0.0))));
    }
    UpdateOutput();
    UpdateMeters();
    UpdateVariables();
    if (!native_menus_)
    {
        DrawMenus();
    }
    DrawControlBar();

    // The zones dock in a space that fills the window below the control bar. Without a saved layout, they take the
    // default one. The window's darkest colour shows between them.
    const ImGuiID dockspace = ImGui::GetID("Zones");
    const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace);
    if (reset_layout_ || node == nullptr || !node->IsSplitNode())
    {
        BuildLayout(dockspace);
        reset_layout_ = false;
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kWindow));
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
    ImGui::PopStyleColor();

    // While the reader builds the archive of another version, the mixer's controls and the Key Editor's notes wait for
    // it.
    SongView* song = song_ ? &*song_ : nullptr;
    const bool busy = reading_version_ != nullptr;
    ShowTrackColours();
    ShowSound(sound_section_, history_ ? history_->Current() : nullptr, ShownSound());
    ShowWaveSoundIn(sound_section_);
    DrawInspector(song, master_, arrangement_, sound_section_, busy, &views_.inspector);
    ApplySoundChange(sound_section_);
    ApplyTagChange(sound_section_);
    ApplyWaveSoundChange(sound_section_);
    ShowSound(browser_sound_, history_ ? history_->Current() : nullptr, BrowsedSound());
    ShowWaveSoundIn(browser_sound_);
    DrawBrowser(browser_, file_name_, browser_state_, browser_sound_, &views_.browser);
    ApplySoundChange(browser_sound_);
    ApplyTagChange(browser_sound_);
    ApplyWaveSoundChange(browser_sound_);
    OpenBrowsed();
    if (const std::optional<BrowserPlace> place = std::exchange(browser_state_.export_place, std::nullopt))
    {
        StartItemExport(*place);
    }
    if (const std::optional<BrowserPlace> place = std::exchange(browser_state_.replace_place, std::nullopt))
    {
        StartReplace(*place);
    }

    // The row's editor takes the place of its preview: a double-click's first click started it.
    if (const std::optional<BrowserPlace> place = std::exchange(browser_state_.edit_place, std::nullopt))
    {
        if (browser_state_.playing == *place)
        {
            worker_.Stop();
            browser_state_.playing.reset();
        }
        OpenEditor(*place);
    }

    arrangement_.track_actions = {IsEnabled(Command::kAddTrack), IsEnabled(Command::kDuplicateTracks),
                                  IsEnabled(Command::kRemoveSelectedTracks), IsEnabled(Command::kCopyTrack),
                                  IsEnabled(Command::kPasteTrack)};

    // Without a sequence sound in the browser, the hint names the menus that add one, or else the File menu.
    if (Document() == nullptr)
    {
        arrangement_.empty_hint = "Open a file, or import a sound archive, with the File menu.";
    }
    else if (browser_items_[kSequencesGroup].empty())
    {
        arrangement_.empty_hint =
            "The file holds no sequence sound. Project > Add Sequence Sound… adds one, and "
            "File > Import > Sequence… brings one in.";
    }
    else
    {
        arrangement_.empty_hint = "Choose a sequence in the browser to see its tracks here.";
    }
    DrawArrangement(song, transport_, arrangement_, busy);
    ApplyColourPick();
    // A double-click gives the editor the keyboard, also when the open editor shows another track now. The lanes take
    // clicks without an item, and Dear ImGui focuses the window under such a click at the frame's end. The editor
    // therefore takes the focus again at the start of the next frame.
    if (std::exchange(arrangement_.open_editor, false))
    {
        OpenKeyEditor();
        ImGui::SetWindowFocus(kKeyEditor);
        focus_next_frame_ = kKeyEditor;
    }
    if (const std::optional<MarkerPlace> place = std::exchange(arrangement_.open_marker, std::nullopt))
    {
        OpenMarker(*place);
    }
    if (const std::optional<TempoPointView> point = std::exchange(arrangement_.open_tempo, std::nullopt))
    {
        OpenCommands(point->track, std::span(&point->command, 1), point->tick);
    }
    if (const std::optional<TempoMove> move = std::exchange(arrangement_.tempo_move, std::nullopt))
    {
        ApplyTempoMove(*move);
    }
    if (const std::optional<LoopMove> move = std::exchange(arrangement_.loop_move, std::nullopt))
    {
        ApplyLoopMove(*move);
    }
    if (const std::optional<PartEdit> edit = std::exchange(arrangement_.part_edit, std::nullopt))
    {
        ApplyPartEdit(*edit);
    }
    if (const std::optional<TrackAction> action = std::exchange(arrangement_.track_action, std::nullopt))
    {
        EditTracks(*action);
    }

    // The mixer's FX channels show the effects on the aux buses that both the file's console and the engine that plays
    // the shown sound have. They offer the kinds of that engine. An effect of another console's kind stays idle there:
    // the engine's model leaves it out. A click in the Undo History moves through it.
    const BusEffects* effects = Effects();
    const format::Engine played = PlayedEngine();
    if (effects != nullptr)
    {
        fx_.count = std::min(FxChannelsOf(Document()->engine), FxChannelsOf(played));
        fx_.kinds = EffectKindNames(*Document(), played);
        fx_.idle_note = std::string("The ") + EngineName(played) + "'s engine doesn't run this effect.";
    }
    for (std::size_t channel = 0; effects != nullptr && channel < effects->size(); channel++)
    {
        fx_.effects[channel].clear();
        fx_.idle[channel].clear();
        for (const format::Effect& effect : (*effects)[channel])
        {
            fx_.effects[channel].emplace_back(EffectName(effect));
            fx_.idle[channel].push_back(!edit::RunsEffect(played, effect.kind));
        }
    }
    fx_.open = effect_window_;
    FollowBusVolumes();

    // The mixer's switches show the shown sound's mixing mode and engine. They refuse a mode that Hokuto doesn't play
    // on the engine, and an engine that Hokuto can't play the sound on. The strips show the sends that both the file's
    // console's tracks and the engine's have.
    if (song != nullptr)
    {
        const format::File* document = Document();
        const format::Engine engine = ShownEngine();
        song->enhanced = ShownMixing() == song::Mixing::kEnhanced;
        song->enhanced_refused =
            document != nullptr ? format::MixingRefused(engine, format::Mixing::kEnhanced).value_or("") : "";
        song->sends = kSendCount;
        const std::optional<ntdlib::sar::Flavor> flavor = format::FlavorOf(engine);
        const auto sends = [&](uint8_t bus)
        {
            return song::Runs(edit::ConsoleOf(*document), ntdlib::commands::Kind::kSend, bus) &&
                   (!flavor || song::Runs(*flavor, ntdlib::commands::Kind::kSend, bus));
        };
        while (document != nullptr && document->native && song->sends > 0 &&
               !sends(static_cast<uint8_t>(song->sends - 1)))
        {
            song->sends--;
        }
        song->engines = document != nullptr && document->native && sequence_ != nullptr && !preview_;
        song->engine = static_cast<int>(engine);
        song->file_engine = document != nullptr ? static_cast<int>(document->engine) : 0;
        for (std::size_t i = 0; document != nullptr && i < song->engine_refused.size(); i++)
        {
            song->engine_refused[i] = edit::EngineRefused(*document, static_cast<format::Engine>(i)).value_or("");
        }
        song->engine_notes =
            engine_sound_ && PlayedEngine() == engine ? engine_sound_->warnings : std::vector<std::string>();
        song->engine_error = engine_failed_ ? engine_error_ : "";
        song->failed_engine = engine_failed_ ? static_cast<int>(*engine_failed_) : -1;
    }
    if (FrontOfLowerZone() == LowerZone::kMixer)
    {
        DrawMixer(song, master_, arrangement_, effects != nullptr ? &fx_ : nullptr, busy, &views_.mixer);
    }
    ApplyFxMove();
    ApplyMixMoves();
    ApplyInstrumentChoices();
    DrawEffects();
    UpdateBankEditor();
    UpdateProperties();
    UpdateKeyEditor();
    UpdateListEditor();
    UpdateScenes();
    DrawVariables(song, variables_state_, &views_.variables);
    ApplyVariableChoices();
    ApplySceneRequest();
    ApplyChangeRequest();
    ApplyModeRequest();
    ApplyMixingRequest();
    ApplyEngineRequest();
    const std::optional<std::size_t> chosen_place =
        DrawUndoHistory(history_rows_, history_ ? history_->Done() : 0, &views_.undo_history);
    if (chosen_place && history_)
    {
        const std::size_t done = history_->Done();
        const std::shared_ptr<const format::File> left = history_->Current();
        history_->GoTo(*chosen_place);
        FollowHistory(done, *left);
    }
    DrawStudioSetup();
    DrawPreferences();
    DrawProjectSetup();
    DrawNewProject();
    DrawExport();
    DrawReplace();
    DrawOpening();

    // While a sequence is being read, the pointer shows that the program is busy. The window still takes clicks.
    if (reader_.Reading())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Progress);
    }

    // The settings keep which zones show.
    if (views_ != saved_views_)
    {
        saved_views_ = views_;
        ImGui::MarkIniSettingsDirty();
    }

    RunWaitingCommands();
    ImGui::Render();
    Present();
    Pace();

    return quit_ ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void App::HandleShortcuts()
{
    // Escape closes the menu, the context menu or the list that's open on top. Every system does the same. Dear ImGui
    // does it only with keyboard navigation. Hokuto leaves keyboard navigation off. The windows that wait for an answer
    // take Escape themselves. The key closes the popup alone: nothing else takes it in this frame.
    ImGuiContext& context = *ImGui::GetCurrentContext();
    if (!context.OpenPopupStack.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        const ImGuiWindow* top = context.OpenPopupStack.back().Window;
        if (top == nullptr || (top->Flags & ImGuiWindowFlags_Modal) == 0)
        {
            ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
            ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##escape"), ImGuiInputFlags_LockThisFrame);
        }
    }

    // The menus' shortcuts work wherever the mouse is. A text field that's being typed in keeps the keys that type. On
    // the Mac, the menu bar handles the keys with Command.
    const bool typing = ImGui::GetIO().WantTextInput;
    const auto handle = [this, typing](const MenuItem& item)
    {
        if (item.keys == ImGuiKey_None || (native_menus_ && (item.keys & ImGuiMod_Ctrl) != 0) ||
            (typing && TypesText(item.keys)) || !IsEnabled(item.command))
        {
            return;
        }

        const ImGuiInputFlags flags = ImGuiInputFlags_RouteGlobal | (item.repeat ? ImGuiInputFlags_Repeat : 0);
        if (ImGui::Shortcut(item.keys, flags))
        {
            Execute(item.command);
        }
    };
    for (const Menu& menu : Menus())
    {
        VisitItems(menu.items, handle);
    }

    // Return goes to the start too. Keyboards without a keypad need it.
    if (!typing && ImGui::Shortcut(ImGuiKey_Enter, ImGuiInputFlags_RouteGlobal))
    {
        Execute(Command::kGoToStart);
    }
}

void App::DrawMenus()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(10.0f), Px(6.0f)));
    const bool open = ImGui::BeginMainMenuBar();
    ImGui::PopStyleVar();
    if (!open)
    {
        return;
    }

    // A window that waits for an answer, such as the Replace window or the Opening window, keeps the clicks to itself.
    // The menus in the program's window then show what they do: nothing. They grey. The Mac's menu bar greys its items
    // too. The shortcuts of the commands that wait for no window still run.
    const bool waits = ImGui::GetTopMostPopupModal() != nullptr;
    if (waits)
    {
        ImGui::BeginDisabled();
    }
    for (const Menu& menu : Menus())
    {
        if (ImGui::BeginMenu(menu.name))
        {
            DrawMenuItems(menu.items);
            ImGui::EndMenu();
        }
    }
    if (waits)
    {
        ImGui::EndDisabled();
    }

    ImGui::EndMainMenuBar();
}

void App::DrawMenuItems(const std::vector<MenuItem>& items)
{
    for (const MenuItem& item : items)
    {
        if (item.name == nullptr)
        {
            ImGui::Separator();
            continue;
        }

        if (!item.items.empty())
        {
            if (ImGui::BeginMenu(item.name))
            {
                DrawMenuItems(item.items);
                ImGui::EndMenu();
            }
            continue;
        }

        // A listed submenu's items come from Listed each time that it opens. On the Mac, the menu bar's submenus take
        // their items at each opening too.
        if (item.none != nullptr)
        {
            if (ImGui::BeginMenu(item.name))
            {
                const std::vector<std::string> titles = Listed(item.command);
                if (titles.empty())
                {
                    ImGui::MenuItem(item.none, nullptr, false, false);
                }
                for (std::size_t i = 0; i < titles.size(); i++)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::MenuItem(titles[i].c_str(), nullptr, false, IsEnabled(item.command)))
                    {
                        ExecuteListed(item.command, i);
                    }
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            continue;
        }

        const std::string title = Title(item.command, item.name);
        const std::string keys = item.keys == ImGuiKey_None ? std::string() : ShortcutText(item.keys);
        if (ImGui::MenuItem(title.c_str(), keys.empty() ? nullptr : keys.c_str(), IsChecked(item.command),
                            IsEnabled(item.command)))
        {
            Execute(item.command);
        }
    }
}

bool App::WaitForTyping(std::function<void()> run)
{
    // The field keeps its text: it ends in the next frame that draws it. A click elsewhere does the same. A save, an
    // export or a question about the changes then includes the text. A minimized window draws no frames. It comes back,
    // and the field ends there.
    if (ImGui::GetInputTextState(ImGui::GetActiveID()) == nullptr)
    {
        return false;
    }

    ImGui::ClearActiveID();
    waiting_commands_.push_back({.run = std::move(run), .frame = ImGui::GetFrameCount()});
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        SDL_RestoreWindow(window_);
    }

    return true;
}

void App::RunWaitingCommands()
{
    // The commands run at the end of a frame after the one in which they came, once no field is being typed in. A field
    // that the user has started since then ends too.
    if (waiting_commands_.empty())
    {
        return;
    }
    if (ImGui::GetInputTextState(ImGui::GetActiveID()) != nullptr)
    {
        ImGui::ClearActiveID();
        return;
    }
    if (waiting_commands_.front().frame >= ImGui::GetFrameCount())
    {
        return;
    }

    for (const WaitingCommand& waiting : std::exchange(waiting_commands_, {}))
    {
        waiting.run();
    }
}

void App::Execute(Command command)
{
    // A command from the Mac's menu bar comes outside SDL3's events. The wake ends any wait for an event. The next
    // frame then shows the command's result, or ends the program after Quit.
    Wake();

    // A command whose menu item is grey does nothing, whatever gave it. A text field that's being typed in keeps its
    // text before the command runs.
    if (!IsEnabled(command) || WaitForTyping([this, command] { Execute(command); }))
    {
        return;
    }

    // Open, Import and Quit close the document. The user decides about its changes first.
    switch (command)
    {
    // The window opens with the Switch chosen each time. Cancel drops the choice.
    case Command::kNewProject:
        new_project_open_ = true;
        new_project_engine_ = format::Engine::kSwitch;
        break;

    case Command::kOpen:
        AfterChanges([this] { ShowDialog(Dialog::kOpen); });
        break;

    case Command::kSave:
        Save();
        break;

    case Command::kSaveAs:
        ShowDialog(Dialog::kSaveAs);
        break;

    case Command::kImportSoundArchive:
        AfterChanges([this] { ShowDialog(Dialog::kImport); });
        break;

    case Command::kImportSequence:
        StartSequenceImport();
        break;

    case Command::kImportBank:
        ShowDialog(Dialog::kImportBank);
        break;

    case Command::kImportWaveArchive:
        ShowDialog(Dialog::kImportWaveArchive);
        break;

    case Command::kImportWave:
        ShowDialog(Dialog::kImportWave);
        break;

    // The new wave sounds copy the settings of a wave sound or a sequence sound of the archive. Without one, the import
    // says so before the dialog.
    case Command::kImportWaveSoundFile:
        if (wii_archive_
                ? HoldsSoundOf(*wii_archive_, {ntdlib::rsar::SoundType::kWaveSound, ntdlib::rsar::SoundType::kSequence})
                : HoldsSoundOf(*archive_, {ntdlib::sar::SoundType::kWaveSound, ntdlib::sar::SoundType::kSequence}))
        {
            ShowDialog(Dialog::kImportWaveSoundFile);
        }
        else
        {
            ShowMessage(
                "Hokuto can't import a wave sound file here yet. The new sounds copy the settings of a wave sound or "
                "a sequence sound, and the archive holds neither.");
        }
        break;

    case Command::kImportAudioFile:
        ShowDialog(Dialog::kImportAudioFile);
        break;

    case Command::kImportStream:
        ShowDialog(Dialog::kImportStream);
        break;

    case Command::kExportSoundArchive:
        StartExport(ExportTarget::kSoundArchive);
        break;

    case Command::kExportMidiSoundFont:
        StartExport(ExportTarget::kMidiSoundFont);
        break;

    case Command::kExportMidiSfz:
        StartExport(ExportTarget::kMidiSfz);
        break;

    case Command::kExportOtherConsole:
        // A sound of the Wii goes to the Wii U first.
        if (const format::File* document = Document(); document != nullptr && document->native)
        {
            StartExport(SongTargetFor(format::EngineOf(
                document->native->wii ? ntdlib::sar::Flavor::kCafe : song::OtherConsole(document->native->flavor))));
        }
        break;

    // The submenu's items open the files (ExecuteListed).
    case Command::kRecentProjects:
        break;

    case Command::kQuit:
        AfterChanges([this] { quit_ = true; });
        break;

    case Command::kUndo:
        if (history_)
        {
            const std::size_t done = history_->Done();
            const std::shared_ptr<const format::File> left = history_->Current();
            history_->Undo();
            FollowHistory(done, *left);
        }
        break;

    case Command::kRedo:
        if (history_)
        {
            const std::size_t done = history_->Done();
            const std::shared_ptr<const format::File> left = history_->Current();
            history_->Redo();
            FollowHistory(done, *left);
        }
        break;

    case Command::kUndoHistory:
        views_.undo_history = !views_.undo_history;
        break;

    // While another window holds the lower zone in front of an open Key Editor, the command brings the editor to the
    // front.
    case Command::kOpenCloseEditor:
        if (key_editor_open_ && FrontOfLowerZone() != LowerZone::kKeyEditor)
        {
            lower_front_ = LowerZone::kKeyEditor;
            ImGui::SetWindowFocus(kKeyEditor);
        }
        else if (key_editor_open_)
        {
            CloseKeyEditor();
        }
        else
        {
            OpenKeyEditor();
        }
        break;

    // Open List Editor shows the selected part's track, or else the selected track, in front of the lower zone. An
    // editor that shows that track already comes to the front as it is, unless a part is selected: the editor then goes
    // to the part. The command never closes the editor.
    case Command::kOpenListEditor:
        if (list_editor_open_ && sequence_ && ListEditorChoice().first == list_editor_track_ &&
            !ListEditorChoice().second)
        {
            lower_front_ = LowerZone::kListEditor;
            ImGui::SetWindowFocus(kListEditor);
        }
        else
        {
            OpenListEditor();
        }
        break;

    // Quantize, Cut, Copy and Paste wait for the Key Editor's next frame.
    case Command::kQuantize:
        key_editor_.request = KeyRequest::kQuantize;
        break;

    case Command::kCut:
        key_editor_.request = KeyRequest::kCut;
        break;

    case Command::kCopy:
        key_editor_.request = KeyRequest::kCopy;
        break;

    case Command::kPaste:
        key_editor_.request = KeyRequest::kPaste;
        break;

    // Select All selects every note of the Key Editor, or every command of the List Editor: the editor that has the
    // focus.
    case Command::kSelectAll:
        if (key_editor_open_ && key_editor_.focused)
        {
            key_editor_.request = KeyRequest::kSelectAll;
        }
        else if (list_editor_open_ && list_editor_.focused)
        {
            list_editor_.select_all = true;
        }
        break;

    // While the Key Editor has the focus, the zoom commands zoom the editor.
    case Command::kZoomIn:
        ++(key_editor_.focused ? key_editor_.zoom_steps : arrangement_.zoom_steps);
        break;

    case Command::kZoomOut:
        --(key_editor_.focused ? key_editor_.zoom_steps : arrangement_.zoom_steps);
        break;

    case Command::kZoomFull:
        (key_editor_.focused ? key_editor_.zoom_full : arrangement_.zoom_full) = true;
        break;

    case Command::kZoomInVertically:
        ++(key_editor_.focused ? key_editor_.vertical_zoom_steps : arrangement_.vertical_zoom_steps);
        break;

    case Command::kZoomOutVertically:
        --(key_editor_.focused ? key_editor_.vertical_zoom_steps : arrangement_.vertical_zoom_steps);
        break;

    case Command::kAddSequenceSound:
        StartSoundAddition();
        break;

    case Command::kAddBank:
        StartBankAddition();
        break;

    case Command::kAddTrack:
        EditTracks(TrackAction::kAdd);
        break;

    case Command::kDuplicateTracks:
        EditTracks(TrackAction::kDuplicate);
        break;

    case Command::kRemoveSelectedTracks:
        EditTracks(TrackAction::kRemove);
        break;

    case Command::kCopyTrack:
        EditTracks(TrackAction::kCopy);
        break;

    case Command::kPasteTrack:
        EditTracks(TrackAction::kPaste);
        break;

    case Command::kTempoTrack:
        arrangement_.tempo_row = !arrangement_.tempo_row;
        break;

    // With a sequence shown, a start ends the Replace or Import window's Play: playback takes the preview's place.
    // Without one, a start ends the window's Play alone, and the empty song stays stopped. The browser's previews play
    // on under the empty song.
    case Command::kStartStop:
        if (!sequence_ && replace_previewing_ && !transport_.Playing())
        {
            StopPreview();
            break;
        }

        transport_.StartStop();
        break;

    case Command::kStart:
        if (!sequence_ && replace_previewing_ && !transport_.Playing())
        {
            StopPreview();
            break;
        }

        transport_.Start();
        break;

    case Command::kStop:
        transport_.Stop();
        break;

    case Command::kCycle:
        transport_.SetCycle(!transport_.Cycle());
        break;

    // The locators go round the selected part. Cycle stays as it is.
    case Command::kLocatorsToSelection:
        if (const PartView* part = SelectedPart(song_, arrangement_))
        {
            const double quarter = song_->ticks_per_quarter;
            transport_.SetLocators(part->start / quarter, (part->start + part->length) / quarter);
        }
        break;

    case Command::kGoToStart:
        transport_.GoToStart();
        break;

    case Command::kGoToLeftLocator:
        transport_.GoToLeftLocator();
        break;

    case Command::kGoToRightLocator:
        transport_.GoToRightLocator();
        break;

    case Command::kLocateNextEvent:
        transport_.Locate(PartBounds(song_, arrangement_), 1);
        break;

    case Command::kLocatePreviousEvent:
        transport_.Locate(PartBounds(song_, arrangement_), -1);
        break;

    case Command::kRewind:
        transport_.Step(-1);
        break;

    case Command::kForward:
        transport_.Step(1);
        break;

    // While the Key Editor or the List Editor holds the lower zone, the mixer's command brings the mixer to the front
    // instead of closing it. When the mixer closes, the editor that it came in front of comes back.
    case Command::kMixer:
        {
            const LowerZone front = FrontOfLowerZone();
            if (views_.mixer && front != LowerZone::kMixer)
            {
                behind_mixer_ = front;
                lower_front_ = LowerZone::kMixer;
                ImGui::SetWindowFocus(kMixer);
            }
            else
            {
                views_.mixer = !views_.mixer;
                if (views_.mixer)
                {
                    behind_mixer_ = front;
                    lower_front_ = LowerZone::kMixer;
                    ImGui::SetWindowFocus(kMixer);
                }
                else
                {
                    lower_front_ = behind_mixer_;
                }
            }
            break;
        }

    case Command::kStudioSetup:
        studio_setup_open_ = true;
        break;

    case Command::kProjectSetup:
        project_setup_open_ = true;
        break;

    case Command::kPreferences:
        preferences_open_ = true;
        break;

    case Command::kInspector:
        views_.inspector = !views_.inspector;
        break;

    case Command::kBrowser:
        views_.browser = !views_.browser;
        break;

    case Command::kVariables:
        views_.variables = !views_.variables;
        break;

    // The zones, the zoom and the tracks' height go back to how a first run shows them.
    case Command::kResetLayout:
        views_ = OpenViews();
        arrangement_.beat_width = kDefaultBeatWidth;
        arrangement_.track_height = kDefaultTrackHeight;
        arrangement_.track_list_width = kDefaultTrackListWidth;
        reset_layout_ = true;
        break;
    }
}

bool App::IsChecked(Command command) const
{
    switch (command)
    {
    case Command::kUndoHistory:
        return views_.undo_history;
    case Command::kCycle:
        return transport_.Cycle();
    case Command::kMixer:
        return views_.mixer;
    case Command::kInspector:
        return views_.inspector;
    case Command::kBrowser:
        return views_.browser;
    case Command::kVariables:
        return views_.variables;
    case Command::kTempoTrack:
        return arrangement_.tempo_row;
    default:
        return false;
    }
}

bool App::IsEnabled(Command command) const
{
    // While a file opens, every command but Quit waits for it.
    if (opening_ && command != Command::kQuit)
    {
        return false;
    }

    // Save and Save As wait for a file to save. The made-up song isn't one. Project Setup waits for one too. An export
    // waits for a file with a sound archive, and MIDI for a sequence in the zones. One export goes at a time. Undo and
    // Redo wait for an action to undo or redo. A text field that's being typed in keeps their keys for its text. The
    // Replace window makes its edit from the version that it opened on, and Undo and Redo wait while it's open. An
    // import of a sequence, a bank, a wave archive or a wave sound file waits for a file with the archive's tables, for
    // the Export and Replace windows to close, and for the archive of the last edit. So do the Project menu's
    // additions. Locators to Selection waits for a selected part. The Key Editor opens on a part or a track of a
    // sequence: the made-up song's have no commands to edit. Quantize waits for the Key Editor to have the focus and to
    // take edits. So do Cut, Copy and Paste, with notes to take or to paste. Select All waits for the Key Editor or the
    // List Editor to have the focus. Duplicate Tracks and Remove Selected Tracks wait for a selected track. Track 0
    // starts the others, and stays. While a window waits for an answer, the commands that open another such window or
    // change the file wait for it to close (WindowWaits).
    if (WindowWaits())
    {
        switch (command)
        {
        case Command::kNewProject:
        case Command::kOpen:
        case Command::kSave:
        case Command::kSaveAs:
        case Command::kImportSoundArchive:
        case Command::kImportSequence:
        case Command::kImportBank:
        case Command::kImportWaveArchive:
        case Command::kImportWave:
        case Command::kImportWaveSoundFile:
        case Command::kImportAudioFile:
        case Command::kImportStream:
        case Command::kExportSoundArchive:
        case Command::kExportMidiSoundFont:
        case Command::kExportMidiSfz:
        case Command::kExportOtherConsole:
        case Command::kRecentProjects:
        case Command::kUndo:
        case Command::kRedo:
        case Command::kCut:
        case Command::kPaste:
        case Command::kQuantize:
        case Command::kAddSequenceSound:
        case Command::kAddBank:
        case Command::kProjectSetup:
            return false;
        default:
            break;
        }
    }
    const format::File* document = Document();
    const bool exports = history_ && !preview_ && document != nullptr && document->native && !export_;
    const bool typing = ImGui::GetIO().WantTextInput;
    switch (command)
    {
    case Command::kSave:
    case Command::kSaveAs:
    case Command::kProjectSetup:
        return history_ && !preview_;
    case Command::kExportSoundArchive:
        return exports;
    case Command::kImportSequence:
    case Command::kImportBank:
    case Command::kImportWaveArchive:
    case Command::kImportWaveSoundFile:
    case Command::kAddSequenceSound:
    case Command::kAddBank:
        return history_ && !preview_ && (archive_ || wii_archive_) && document != nullptr && document->native &&
               document->native->tables && !replace_ && !export_ && reading_version_ == nullptr;
    case Command::kImportWave:
    case Command::kImportAudioFile:
        return history_ && !preview_ && document != nullptr && document->native && !replace_ && !export_ &&
               (archive_ ? !archive_->WaveArchives().empty() : wii_archive_ && HoldsWaves(*wii_archive_));
    case Command::kImportStream:
        return history_ && !preview_ && document != nullptr && document->native && !replace_ && !export_ &&
               ((archive_ && document->native->tables && !archive_->Sounds().empty()) ||
                (wii_archive_ && !wii_archive_->Sounds().empty()));
    case Command::kExportMidiSoundFont:
    case Command::kExportMidiSfz:
        return exports && sequence_ && ((archive_ && archive_version_) || wii_archive_);
    case Command::kExportOtherConsole:
        return exports && sequence_ && (archive_ || wii_archive_) && archive_version_ && document->native->tables;
    case Command::kUndo:
        return history_ && history_->CanUndo() && !typing && !replace_;
    case Command::kRedo:
        return history_ && history_->CanRedo() && !typing && !replace_;
    case Command::kLocatorsToSelection:
        return SelectedPart(song_, arrangement_) != nullptr;
    case Command::kOpenCloseEditor:
        return key_editor_open_ ||
               (sequence_ && (SelectedPart(song_, arrangement_) != nullptr ||
                              (arrangement_.selected_track >= 0 &&
                               static_cast<std::size_t>(arrangement_.selected_track) < sequence_->tracks.size())));
    case Command::kOpenListEditor:
        return list_editor_open_ || sequence_ != nullptr;
    case Command::kQuantize:
        return key_editor_open_ && key_editor_.focused && reading_version_ == nullptr;
    case Command::kCut:
        return key_editor_open_ && key_editor_.focused && !key_editor_.selected.empty() && !typing &&
               reading_version_ == nullptr;
    case Command::kCopy:
        return key_editor_open_ && key_editor_.focused && !key_editor_.selected.empty() && !typing;
    case Command::kPaste:
        return key_editor_open_ && key_editor_.focused && !key_editor_.clipboard.empty() && !typing &&
               reading_version_ == nullptr;
    case Command::kSelectAll:
        return !typing && ((key_editor_open_ && key_editor_.focused) || (list_editor_open_ && list_editor_.focused));
    case Command::kAddTrack:
        return TracksEditable();
    case Command::kDuplicateTracks:
        return TracksEditable() && SelectedTrack() != nullptr;
    case Command::kRemoveSelectedTracks:
        return TracksEditable() && SelectedTrack() != nullptr && SelectedTrack()->number != 0;
    case Command::kCopyTrack:
        return TracksEditable() && SelectedTrack() != nullptr;
    case Command::kPasteTrack:
        return TracksEditable() && copied_track_.has_value();
    default:
        return true;
    }
}

std::vector<std::string> App::Listed(Command command) const
{
    return command == Command::kRecentProjects ? RecentProjectTitles(settings_.recent_projects)
                                               : std::vector<std::string>();
}

void App::ExecuteListed(Command command, std::size_t index)
{
    // A recent project opens as through File > Open, once the user has decided about the document's changes.
    Wake();
    if (command != Command::kRecentProjects || index >= settings_.recent_projects.size() || !IsEnabled(command) ||
        WaitForTyping([this, command, index] { ExecuteListed(command, index); }))
    {
        return;
    }

    const std::string path = settings_.recent_projects[index];
    AfterChanges([this, path] { OpenOrImport(path); });
}

std::string App::Title(Command command, std::string_view name) const
{
    std::string title(name);
    if (command == Command::kUndo && history_ && history_->CanUndo())
    {
        title += " " + history_->Actions()[history_->Done() - 1].name;
    }
    else if (command == Command::kRedo && history_ && history_->CanRedo())
    {
        title += " " + history_->Actions()[history_->Done()].name;
    }

    return title;
}

void App::DrawControlBar()
{
    // The bar takes its height from the top of the main window's work area. The menu bar does the same.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::BeginViewportSideBar("##ControlBar", ImGui::GetMainViewport(), ImGuiDir_Up, Px(kControlBarHeight),
                                    kFlags))
    {
        // The mixer's button runs the mixer's command: it brings an open mixer to the front of the lower zone too.
        const bool mixer = views_.mixer;
        app::DrawControlBar(views_, transport_, PositionTime(), master_, studio_setup_open_);
        if (views_.mixer != mixer)
        {
            views_.mixer = mixer;
            Execute(Command::kMixer);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void App::DrawStudioSetup()
{
    if (!studio_setup_open_)
    {
        return;
    }

    // A window over the middle of the main window. The labels sit on the left.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(500.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin(kStudioSetup, &studio_setup_open_, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        return;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. The New Project window
    // does the same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        studio_setup_open_ = false;
    }

    // The audio device.
    const ImVec2 heading = ImGui::GetCursorScreenPos();
    DrawCaps(ImGui::GetWindowDrawList(), heading, theme::kTextMuted, "Audio System");
    ImGui::Dummy(CapsSize("Audio System"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##audio", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Output device");
        DrawDeviceCombo();

        // The format that SDL3 and the system settled on. Hokuto takes their buffer size.
        const int rate = output_.DeviceRate();
        const int frames = output_.DeviceBufferFrames();
        if (output_.IsOpen() && rate > 0 && frames > 0)
        {
            SettingLabel("Sample rate");
            ImGui::Text("%.1f kHz", rate / 1000.0);
            SettingLabel("Buffer size");
            ImGui::Text("%d samples, %.1f ms", frames, 1000.0 * frames / rate);
        }
        else if (!output_.IsOpen())
        {
            SettingLabel("Status");
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kSolo), "No device: %s", output_.Error().c_str());
        }

        SettingLabel("Test tone");
        if (PrimaryButton("Play"))
        {
            PlayTestTone();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void App::DrawProjectSetup()
{
    const format::File* document = Document();
    if (document == nullptr || preview_)
    {
        project_setup_open_ = false;
    }
    if (!project_setup_open_)
    {
        return;
    }

    // A window over the middle of the main window, like Studio Setup.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(500.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin(kProjectSetup, &project_setup_open_, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        return;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. The New Project window
    // does the same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        project_setup_open_ = false;
    }

    // The console whose rules the file plays by, and how its sounds mix. The file's engine can't change. The mixer's
    // switch plays the shown sound on another console's engine. A choice of a mixing mode gives it to every sound. The
    // mixer's switch gives the shown sound another. A mode that Hokuto doesn't play on the file's console shows grey,
    // and its tooltip says why.
    DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, "Project");
    ImGui::Dummy(CapsSize("Project"));
    ImGui::Spacing();
    std::optional<format::Mixing> mixing;
    if (ImGui::BeginTable("##project", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Engine");
        ImGui::TextUnformatted(EngineName(document->engine));
        if (document->native && document->native->wii)
        {
            // The import reads the game's build from its main.dol on the extracted disc.
            SettingLabel("Sound library");
            TextWrappedAtSpaces(WiiBuildText(*document->native));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
            TextWrappedAtSpaces(
                "The build decides how the sequences play. An import reads it from sys/main.dol of the extracted "
                "disc that holds the archive.");
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
            ImGui::TextWrapped("The mixer's engine switch plays a sound on another console's engine.");
            ImGui::PopStyleColor();
        }
        SettingLabel("Mixing");
        constexpr std::array<std::pair<format::Mixing, const char*>, 2> kModes = {
            {{format::Mixing::kStrict, "Strict"}, {format::Mixing::kEnhanced, "Enhanced"}}};
        const bool shared = document->sound_mixing.empty();
        const char* shown = !shared                                         ? "Varies by sound"
                            : document->mixing == format::Mixing::kEnhanced ? "Enhanced"
                                                                            : "Strict";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##mixing", shown))
        {
            for (const auto& [mode, name] : kModes)
            {
                const std::optional<std::string> refused = format::MixingRefused(document->engine, mode);
                if (ImGui::Selectable(name, shared && document->mixing == mode,
                                      refused ? ImGuiSelectableFlags_Disabled : ImGuiSelectableFlags_None))
                {
                    mixing = mode;
                }
                if (refused)
                {
                    ItemTooltip(*refused);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        std::string about =
            "The mode of every sound. Strict mixing plays a sound as the console does. Enhanced mixing lifts the "
            "console's limits. The mixer's switch sets the mode of the shown sound.";
        if (const std::optional<std::string> refused =
                format::MixingRefused(document->engine, format::Mixing::kEnhanced))
        {
            about += " " + *refused;
        }
        else if (std::ranges::any_of(
                     document->sound_engines, [document](const auto& sound)
                     { return sound.second == format::Engine::kWii && !edit::EngineRefused(*document, sound.second); }))
        {
            about += " A sound on the Wii's engine plays in strict mixing.";
        }
        ImGui::TextWrapped("%s", about.c_str());
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }

    // The file's tags, a box of text for each key that the program offers. A box that the user empties removes its tag.
    ImGui::Spacing();
    ImGui::Spacing();
    DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, "Tags");
    ImGui::Dummy(CapsSize("Tags"));
    ImGui::Spacing();
    std::optional<std::pair<std::string, std::string>> change;
    if (ImGui::BeginTable("##tags", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        for (const std::string_view key_view : format::kTagKeys)
        {
            const std::string key(key_view);
            const std::string label = TagLabel(key);
            SettingLabel(label.c_str());
            ItemTooltip(TagTip(key));
            const auto found = document->tags.find(key);
            const std::string_view value = found != document->tags.end() ? std::string_view(found->second) : "";
            std::optional<std::string> typed = TextEntry(key.c_str(), value, ImGui::GetCursorScreenPos(),
                                                         ImGui::GetContentRegionAvail().x, project_typing_);
            if (typed && *typed != value)
            {
                change.emplace(key, std::move(*typed));
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();

    if (change)
    {
        Do("Change Tag", "File: " + file_name_ + " - " + TagLabel(change->first),
           format::WithTag(*document, std::nullopt, change->first, change->second));
    }
    else if (mixing && (!document->sound_mixing.empty() || *mixing != document->mixing))
    {
        Do(*mixing == format::Mixing::kEnhanced ? "Enhanced Mixing" : "Strict Mixing", "File: " + file_name_,
           format::WithMixing(*document, *mixing));
    }
}

void App::DrawNewProject()
{
    if (!new_project_open_)
    {
        return;
    }

    // A window over the middle of the main window, like Project Setup.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(500.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin(kNewProject, &new_project_open_, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        return;
    }

    // Escape cancels while the window has the focus, unless a text field is being typed in. The Import window does the
    // same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        new_project_open_ = false;
    }

    // The console whose rules the new file plays by. Project Setup shows it as the engine.
    DrawCaps(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), theme::kTextMuted, "Project");
    ImGui::Dummy(CapsSize("Project"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##new", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Engine");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto chosen = static_cast<std::size_t>(new_project_engine_);
        if (ImGui::BeginCombo("##engine", kEngineNames[chosen]))
        {
            for (std::size_t i = 0; i < kEngineNames.size(); i++)
            {
                if (ImGui::Selectable(kEngineNames[i], i == chosen))
                {
                    new_project_engine_ = static_cast<format::Engine>(i);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        ImGui::TextWrapped(
            "An empty sound archive of the console. Project > Add Sequence Sound… adds a sound with an empty song. "
            "Banks, wave archives and other items come in through File > Import.");
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }

    // Cancel and Create sit at the right. The Import window's buttons sit there too.
    ImGui::Spacing();
    ImGui::Spacing();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Create").x +
                          style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons);
    bool create = false;
    if (ImGui::Button("Cancel"))
    {
        new_project_open_ = false;
    }
    ImGui::SameLine();
    if (PrimaryButton("Create"))
    {
        create = true;
    }
    ImGui::End();

    // The user decides about the document's changes once Create replaces it, edits made meanwhile included.
    if (create)
    {
        new_project_open_ = false;
        AfterChanges([this, engine = new_project_engine_] { NewDocument(engine); });
    }
}

LowerZone App::FrontOfLowerZone() const
{
    const bool key_editor = key_editor_open_ && sequence_;
    const bool list_editor = list_editor_open_ && sequence_;
    if ((lower_front_ == LowerZone::kMixer && views_.mixer) || (lower_front_ == LowerZone::kKeyEditor && key_editor) ||
        (lower_front_ == LowerZone::kListEditor && list_editor))
    {
        return lower_front_;
    }

    return key_editor ? LowerZone::kKeyEditor : list_editor ? LowerZone::kListEditor : LowerZone::kMixer;
}

void App::DrawPreferences()
{
    if (!preferences_open_)
    {
        return;
    }

    // A window over the middle of the main window, like Studio Setup.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(500.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::Begin(kPreferences, &preferences_open_, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!visible)
    {
        ImGui::End();
        return;
    }

    // Escape closes the window while it has the focus, unless a text field is being typed in. The New Project window
    // does the same. An open menu of the window takes Escape first (HandleShortcuts).
    if (EscapeCloses())
    {
        preferences_open_ = false;
    }

    // The tracks' colours.
    const ImVec2 heading = ImGui::GetCursorScreenPos();
    DrawCaps(ImGui::GetWindowDrawList(), heading, theme::kTextMuted, "Tracks");
    ImGui::Dummy(CapsSize("Tracks"));
    ImGui::Spacing();
    if (ImGui::BeginTable("##tracks", 2))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, Px(120.0f));
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        SettingLabel("Colours");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const auto chosen = static_cast<std::size_t>(settings_.track_colours);
        if (ImGui::BeginCombo("##colours", kTrackColourNames[chosen]))
        {
            for (std::size_t i = 0; i < kTrackColourNames.size(); i++)
            {
                if (ImGui::Selectable(kTrackColourNames[i], i == chosen))
                {
                    settings_.track_colours = static_cast<TrackColours>(i);
                    ImGui::MarkIniSettingsDirty();
                }
            }
            ImGui::EndCombo();
        }

        // What the choice does.
        constexpr std::array<const char*, 3> kHints = {
            "Each track takes the next colour of the rainbow.",
            "Every track takes the same grey.",
            "Click a track's colour strip, or its colour in the inspector, to choose its colour. The file keeps it.",
        };
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextMuted));
        ImGui::TextWrapped("%s", kHints[static_cast<std::size_t>(settings_.track_colours)]);
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }
    ImGui::End();
}

void App::DrawDeviceCombo()
{
    // Devices can share a name. Their indexes tell them apart.
    const char* preview = settings_.device.empty() ? kSystemDefault : settings_.device.c_str();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##device", preview))
    {
        return;
    }

    if (ImGui::Selectable(kSystemDefault, settings_.device.empty()))
    {
        ChooseDevice("");
    }
    const std::vector<std::string> names = AudioOutput::DeviceNames();
    for (std::size_t i = 0; i < names.size(); i++)
    {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(names[i].c_str(), names[i] == settings_.device))
        {
            ChooseDevice(names[i]);
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

void App::ChooseDevice(const std::string& name)
{
    settings_.device = name;
    output_.Open(name);
    ImGui::MarkIniSettingsDirty();
}

void App::PlayTestTone()
{
    PlayAlone(std::make_unique<audio::TestTone>(kDefaultRate));
}

void App::PlayAlone(std::unique_ptr<audio::Renderer> renderer)
{
    // The renderer takes the place of the sequence's playback, of a preview and of an audition.
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    StopPlayback();
    browser_state_.playing.reset();
    wave_state_.playing = false;
    replace_previewing_ = false;
    audition_worker_.Stop();
    output_.SetSourceRate(renderer->SampleRate());
    output_.Resume();
    worker_.Play(std::move(renderer));
}

void App::PlayReplacePreview(std::unique_ptr<audio::Renderer> renderer)
{
    PlayAlone(std::move(renderer));
    replace_previewing_ = true;
}

void App::StopPreview()
{
    // A preview plays on the worker while no playback follows the transport.
    if (!clock_ && worker_.Playing())
    {
        worker_.Stop();
    }
    browser_state_.playing.reset();
    wave_state_.playing = false;
    replace_previewing_ = false;
}

void App::PlayAudition(std::unique_ptr<audio::Renderer> renderer)
{
    // Playback goes on under the audition, at the same rate. A preview or the test tone stops. Its rate can differ. On
    // the 3DS, the two mixing modes differ in rate. An audition in one mode during playback in the other reaches the
    // playback's rate through a resampler.
    const bool playing = transport_.Playing();
    if (!playing && worker_.Playing())
    {
        worker_.Stop();
        browser_state_.playing.reset();
        wave_state_.playing = false;
        replace_previewing_ = false;
    }
    if (playing)
    {
        renderer = song::AtRate(std::move(renderer), playback_rate_);
    }
    audition_rate_ = renderer->SampleRate();
    if (!playing)
    {
        output_.SetSourceRate(audition_rate_);
    }
    output_.Resume();
    audition_worker_.Play(std::move(renderer));
}

void App::UpdateOutput()
{
    // The Stereo Out fader sets the output's gain. The auditions' lead follows the device.
    output_.SetGain(std::pow(10.0f, master_.volume / 20.0f));
    audition_worker_.SetAhead(AuditionLead(output_.DeviceRate(), output_.DeviceBufferFrames()));
    RestOutput();
}

void App::RestOutput()
{
    // Something sounds while a worker renders or a ring holds frames. A start resumes the device before it renders.
    // Anything else that fills a ring resumes it here.
    const uint64_t now = SDL_GetTicksNS();
    const bool sounding =
        worker_.Playing() || audition_worker_.Playing() || ring_.Available() > 0 || audition_ring_.Available() > 0;
    if (sounding)
    {
        output_sounded_ = now;
        output_.Resume();
        return;
    }
    if (output_.Suspended() || !output_.IsOpen())
    {
        return;
    }

    // The rest. Once SDL3 waits for events, the timer's event brings the program back here when the rest is due.
    const uint64_t due = output_sounded_ + kOutputRest;
    if (now >= due)
    {
        output_.Suspend();
    }
    else if (output_timer_ < due)
    {
        output_timer_ = due;
        const auto wake = [](void* event_type, SDL_TimerID, Uint64) -> Uint64
        {
            SDL_Event event{};
            event.type = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(event_type));
            SDL_PushEvent(&event);

            return 0;
        };
        SDL_AddTimerNS(due - now + kOutputTimerSlack, wake, reinterpret_cast<void*>(uintptr_t{wake_event_}));
    }
}

void App::UpdateMeters()
{
    // The output's meter takes the samples that the device has taken since the last frame.
    const float fall = kMeterFall * ImGui::GetIO().DeltaTime;
    FollowPeaks(output_.TakePeaks(), fall, master_.meter, master_.peak);
    if (!song_)
    {
        return;
    }

    // The meters of the tracks and the FX channels take the samples that the device has played since the last frame.
    song::PlaybackClock::Peaks peaks;
    if (clock_)
    {
        const uint64_t heard = HeardSamples();
        peaks = clock_->PeaksBetween(metered_, heard);
        metered_ = std::max(metered_, heard);
    }

    // A track's number gives its meter in the model. The made-up song of --preview has no model.
    for (std::size_t i = 0; i < song_->tracks.size(); i++)
    {
        TrackView& track = song_->tracks[i];
        const bool modelled = sequence_ && i < sequence_->tracks.size();
        const std::array<float, 2> track_peaks =
            modelled ? peaks.tracks[static_cast<std::size_t>(sequence_->tracks[i].number)] : std::array<float, 2>{};
        FollowPeaks(track_peaks, fall, track.meter, track.peak);
    }
    for (std::size_t channel = 0; channel < kFxChannels; channel++)
    {
        FollowPeaks(peaks.buses[channel], fall, fx_.meters[channel], fx_.peaks[channel]);
    }
}

void App::Pace()
{
    // Playback, the meters and the blinking cursor of a text field move. The window draws on while they move, while a
    // file opens, while a sequence is being read, an export is being prepared or measured, an import or the Replace
    // window's Play builds the archive, a preview plays or a thread measures the groups that load an item, while the
    // user holds a key or a button, and for a moment after each event. The end of such work counts as an event. A
    // window that fits its contents, such as the Export window after a measure, takes its new size a frame or two
    // later.
    const bool tracks_move = song_ && std::any_of(song_->tracks.begin(), song_->tracks.end(),
                                                  [](const TrackView& track) { return Shows(track.meter); });
    const bool fx_move = std::any_of(fx_.meters.begin(), fx_.meters.end(), Shows);

    const auto measuring = [](const std::future<std::vector<std::string>>& sizes)
    {
        return sizes.valid() && !Done(sizes);
    };
    const bool groups_measured = measuring(bank_group_sizes_) || measuring(wave_group_sizes_) ||
                                 measuring(sound_section_.wave_group_sizes) ||
                                 measuring(browser_sound_.wave_group_sizes);
    const bool moving = transport_.Playing() || Shows(master_.meter) || tracks_move || fx_move ||
                        ImGui::GetIO().WantTextInput || reader_.Reading() || (export_ && !export_->done) ||
                        (export_ && export_->measure.valid()) || (replace_ && replace_->pending) ||
                        preview_work_.has_value() || opening_.has_value() || browser_state_.playing.has_value() ||
                        replace_previewing_ || groups_measured;
    if (std::exchange(moving_, moving) && !moving)
    {
        last_event_ = SDL_GetTicksNS();
    }
    SetWaiting(!moving && !Holding() && SDL_GetTicksNS() - last_event_ >= kLinger);
}

void App::SetWaiting(bool waiting)
{
    if (waiting == waiting_)
    {
        return;
    }

    // "0" calls Iterate again at once. Presenting a frame then waits for the display.
    waiting_ = waiting;
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, waiting ? "waitevent" : "0");
}

void App::Wake() const
{
    SDL_Event event{};
    event.type = wake_event_;
    SDL_PushEvent(&event);
}

void App::ShowMessage(const std::string& message, MessageKind kind)
{
    SDL_Log("%s", message.c_str());
    SDL_ShowSimpleMessageBox(kind == MessageKind::kNote ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, "Hokuto",
                             message.c_str(), window_);
}

void App::ShowDialog(Dialog dialog)
{
    // The dialog reads the properties when it opens. The filters have to last until it closes. SDL3 only reads them.
    std::span<const SDL_DialogFileFilter> filters = std::span(kHokutoFilters);
    if (dialog == Dialog::kImport)
    {
        filters = std::span(kArchiveFilters);
    }
    else if (dialog == Dialog::kImportSequence)
    {
        filters = wii_archive_ ? std::span(kWiiSequenceFilters) : std::span(kSequenceFilters);
    }
    else if (dialog == Dialog::kImportBank)
    {
        filters = wii_archive_ ? std::span(kWiiBankFilters) : std::span(kBankFilters);
    }
    else if (dialog == Dialog::kImportWaveArchive)
    {
        filters = wii_archive_ ? std::span(kWiiWaveArchiveFilters) : std::span(kWaveArchiveFilters);
    }
    else if (dialog == Dialog::kImportWaveSoundFile)
    {
        filters = wii_archive_ ? std::span(kWiiWaveSoundFileFilters) : std::span(kWaveSoundFileFilters);
    }
    else if (dialog == Dialog::kImportWave)
    {
        filters = wii_archive_ ? std::span(kWiiWaveFilters) : std::span(kWaveFilters);
    }
    else if (dialog == Dialog::kImportAudioFile)
    {
        filters = std::span(kWavFilters);
    }
    else if (dialog == Dialog::kImportStream)
    {
        filters = wii_archive_ ? std::span(kWiiStreamFilters) : std::span(kStreamFilters);
    }
    else if (dialog == Dialog::kReplace && replace_file_ && wii_archive_)
    {
        filters = replace_file_->kind == edit::FileKind::kBank          ? std::span(kWiiBankFilters)
                  : replace_file_->kind == edit::FileKind::kWaveArchive ? std::span(kWiiWaveArchiveFilters)
                                                                        : std::span(kWiiWaveSoundFileFilters);
    }
    else if (dialog == Dialog::kReplace && replace_sound_ && wii_archive_)
    {
        filters = std::span(kWiiSequenceFilters);
    }
    else if (dialog == Dialog::kReplace && replace_wave_ && wii_archive_)
    {
        filters = std::span(kWiiWaveReplaceFilters);
    }
    else if (dialog == Dialog::kReplace && replace_file_)
    {
        filters = replace_file_->kind == edit::FileKind::kBank          ? std::span(kBankFilters)
                  : replace_file_->kind == edit::FileKind::kWaveArchive ? std::span(kWaveArchiveFilters)
                                                                        : std::span(kWaveSoundFileFilters);
    }
    else if (dialog == Dialog::kReplace && replace_wave_)
    {
        filters = std::span(kWaveReplaceFilters);
    }
    else if (dialog == Dialog::kReplace && replace_stream_ && wii_archive_)
    {
        filters = std::span(kWiiStreamFilters);
    }
    else if (dialog == Dialog::kReplace && replace_stream_ && PlaysAacStream(*replace_stream_))
    {
        filters = std::span(kAacStreamFilters);
    }
    else if (dialog == Dialog::kReplace && replace_stream_)
    {
        filters = std::span(kStreamFilters);
    }
    else if (dialog == Dialog::kReplace)
    {
        filters = std::span(kSequenceFilters);
    }
    else if (dialog == Dialog::kExport && export_ && export_->item)
    {
        // The filter of the item's file, by its extension.
        const std::string extension = xsflib::Utf8(xsflib::PathFromUtf8(export_->file_name).extension());
        filters = {};
        for (const SDL_DialogFileFilter& filter : kItemFilters)
        {
            if (extension == std::string(".") + filter.pattern)
            {
                filters = std::span(&filter, 1);
            }
        }
    }
    else if (dialog == Dialog::kExport && export_ && export_->Target() == ExportTarget::kSoundArchive)
    {
        const format::File* document = Document();
        const format::Engine console = document != nullptr ? document->engine : format::Engine::kSwitch;
        filters = console == format::Engine::k3ds   ? std::span(k3dsArchiveFilters)
                  : console == format::Engine::kWii ? std::span(kWiiArchiveFilters)
                                                    : std::span(kLaterArchiveFilters);
    }
    else if (dialog == Dialog::kExport && export_)
    {
        filters = SongEngineOf(export_->Target()) ? std::span(kHokutoFilters) : std::span(kMidiFilters);
    }
    else if (dialog == Dialog::kExportFolder)
    {
        // A panel of folders takes no filter. On the Mac, a filter of files leaves the panel no folder to choose.
        filters = {};
    }
    const SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                           const_cast<SDL_DialogFileFilter*>(filters.data()));
    SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(filters.size()));
    SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window_);

    SDL_FileDialogType type = SDL_FILEDIALOG_OPENFILE;
    SDL_DialogFileCallback callback = &App::OpenChosen;
    switch (dialog)
    {
    case Dialog::kOpen:
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Open");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Open");
        break;

    // A file that hasn't been saved starts beside the sound archive that it came from, with the archive's name.
    case Dialog::kSaveAs:
        {
            type = SDL_FILEDIALOG_SAVEFILE;
            callback = &App::SaveChosen;
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Save As");
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Save");
            std::filesystem::path location = xsflib::PathFromUtf8(document_path_);
            if (location.empty() && !import_path_.empty())
            {
                location = xsflib::PathFromUtf8(import_path_).replace_extension(".hok");
            }
            if (!location.empty())
            {
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, xsflib::Utf8(location).c_str());
            }
            break;
        }

    case Dialog::kImport:
        callback = &App::ImportChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Sound Archive");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Import");
        break;

    case Dialog::kImportSequence:
        callback = &App::ImportSequenceChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Sequence");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportBank:
        callback = &App::ImportBankChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Bank");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportWaveArchive:
        callback = &App::ImportWaveArchiveChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Wave Archive");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportWave:
        callback = &App::ImportWaveChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Wave");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportWaveSoundFile:
        callback = &App::ImportWaveSoundFileChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Wave Sound File");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportAudioFile:
        callback = &App::ImportAudioFileChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Audio File");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kImportStream:
        callback = &App::ImportStreamChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Import Stream");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    case Dialog::kReplace:
        callback = &App::ReplaceChosen;
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                              replace_file_     ? ReplaceAction(replace_file_->kind).c_str()
                              : replace_wave_   ? "Replace Wave"
                              : replace_stream_ ? "Replace Stream"
                                                : "Replace Sequence");
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
        break;

    // An export starts beside the sound archive or the file that the document came from, with the export's name. A wave
    // archive's WAV files and a bank's SFZ files go into a folder. The dialog then starts in the folder of the archive
    // or the file. That location ends in a separator: SDL3 reads a location without one as a file's name, and the
    // dialog then starts in the folder above.
    case Dialog::kExport:
    case Dialog::kExportFolder:
        {
            const bool folder = dialog == Dialog::kExportFolder;
            const char* title = "Export";
            if (folder)
            {
                title = export_ && export_->Target() == ExportTarget::kSfz ? "Export SFZ Files" : "Export WAV Files";
            }
            type = folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_SAVEFILE;
            callback = folder ? &App::ExportFolderChosen : &App::ExportChosen;
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title);
            SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Export");
            const std::string& source = import_path_.empty() ? document_path_ : import_path_;
            if (export_ && !source.empty())
            {
                const std::filesystem::path beside = xsflib::PathFromUtf8(source).parent_path();
                const std::filesystem::path location =
                    folder ? beside / "" : beside / xsflib::PathFromUtf8(export_->file_name);
                SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, xsflib::Utf8(location).c_str());
            }
            break;
        }
    }

    SDL_ShowFileDialogWithProperties(type, callback, this, properties);
    SDL_DestroyProperties(properties);
}

void SDLCALL App::OpenChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kOpen, files);
}

void SDLCALL App::SaveChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kSaveAs, files);
}

void SDLCALL App::ImportChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImport, files);
}

void SDLCALL App::ImportSequenceChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportSequence, files);
}

void SDLCALL App::ImportBankChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportBank, files);
}

void SDLCALL App::ImportWaveArchiveChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportWaveArchive, files);
}

void SDLCALL App::ImportWaveChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportWave, files);
}

void SDLCALL App::ImportWaveSoundFileChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportWaveSoundFile, files);
}

void SDLCALL App::ImportAudioFileChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportAudioFile, files);
}

void SDLCALL App::ImportStreamChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kImportStream, files);
}

void SDLCALL App::ExportChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kExport, files);
}

void SDLCALL App::ExportFolderChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kExportFolder, files);
}

void SDLCALL App::ReplaceChosen(void* user_data, const char* const* files, int /*filter*/)
{
    static_cast<App*>(user_data)->Choose(Dialog::kReplace, files);
}

void App::Choose(Dialog dialog, const char* const* files)
{
    // No list means an error, and an empty list means the user cancelled. A Save As without a file still reaches the
    // next frame. What waited for the save then goes (AfterChanges).
    const bool chose = files != nullptr && files[0] != nullptr;
    if (!chose && dialog != Dialog::kSaveAs)
    {
        return;
    }

    // The wake makes the program act on the file in its next frame. The path takes the one form that the program keeps:
    // on Windows, the open dialog and the save dialog can name a long path in two forms.
    {
        std::lock_guard lock(chosen_mutex_);
        chosen_.emplace(dialog, chose ? SystemPath(files[0]) : "");
    }
    Wake();
}

void App::DropFilesOnWindow(const std::vector<std::string>& paths)
{
    if (WaitForTyping([this, paths] { DropFilesOnWindow(paths); }))
    {
        return;
    }

    const std::string& path = paths.front();
    const std::string name = xsflib::Utf8(xsflib::PathFromUtf8(path).filename());
    if (WindowWaits())
    {
        ShowMessage("Hokuto opens " + name + " once the window in front closes. Drop the file again then.",
                    MessageKind::kNote);
        return;
    }
    if (!OpensAsDocument(path))
    {
        ShowMessage(name +
                    " is neither a Hokuto file nor a sound archive of the Wii, the 3DS, the Wii U or the "
                    "Switch. A WAV file or a wave file goes on the Bank Editor, or in through File > Import.");
        return;
    }

    if (paths.size() > 1)
    {
        ShowMessage("Hokuto opens one file at a time. It opens " + name + ".", MessageKind::kNote);
    }
    AfterChanges([this, path] { OpenOrImport(path); });
}

void App::OpenOrImport(const std::string& path, bool import)
{
    // The window waits with Cancel while the work runs. A new open drops the work of the one before.
    if (opening_)
    {
        dropped_opens_.push_back(std::move(opening_->work));
    }
    const std::string name = xsflib::Utf8(xsflib::PathFromUtf8(path).filename());
    const auto work = [path, import]
    {
        return Opened(path, import);
    };
    opening_ = Opening{std::async(std::launch::async, work), path, name};
}

App::OpenWork App::Opened(const std::string& path, bool import)
{
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    std::vector<uint8_t> bytes;
    try
    {
        if (!xsflib::ReadFile(file, bytes))
        {
            return {.error = "Hokuto can't read " + xsflib::Utf8(file.filename()) + "."};
        }
    }
    catch (const std::exception& error)
    {
        return {.error =
                    "Hokuto can't read " + xsflib::Utf8(file.filename()) + ". " + format::AsSentence(error.what())};
    }

    // A Hokuto file opens. A sound archive goes in as a new file. The message names the kind of any other file.
    const auto starts = [&bytes](const char* magic)
    {
        return bytes.size() >= 4 && std::memcmp(bytes.data(), magic, 4) == 0;
    };

    if (format::IsBinary(bytes) || format::IsText(bytes))
    {
        return OpenedFile(xsflib::Utf8(file.filename()), bytes);
    }
    if (starts("RSAR"))
    {
        return ImportedWiiArchive(file, std::move(bytes));
    }
    if (starts("CSAR") || starts("FSAR"))
    {
        return ImportedArchive(file, std::move(bytes));
    }

    return {.error = format::NotOpenedText(file, bytes, import)};
}

App::OpenWork App::OpenedFile(const std::string& name, std::span<const uint8_t> bytes)
{
    // The file with its timelines laid out for editing, and the archive that it builds for playback, with the file's
    // group files. The export throws for a file without a native archive. A file of the Wii builds its archive for the
    // Wii's model, and has no group files.
    OpenWork work;
    work.text_form = format::IsText(bytes);
    try
    {
        work.document = std::make_shared<const format::File>(edit::OpenTimelines(format::ReadEitherForm(bytes)));
        const std::vector<uint8_t> built = format::ExportArchive(*work.document);
        if (work.document->native && work.document->native->wii)
        {
            work.wii = song::LoadWiiArchive(built, name, ntdlib::rvl::snd::kRsarOptions);
            work.sequences = format::SequencesOfFiles(*work.document, built, *work.wii);
        }
        else
        {
            work.archive = song::LoadArchive(built, name, format::ExportGroupFiles(*work.document));
            work.sequences = format::SequencesOfFiles(*work.document, built, *work.archive);
        }
        work.start_fields = StartFieldsOf(*work.document, built);
    }
    catch (const xsflib::FormatError& error)
    {
        return {.error = "Hokuto can't open " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::invalid_argument& error)
    {
        return {.error = "Hokuto can't play " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::runtime_error& error)
    {
        return {.error = error.what()};
    }
    catch (const std::exception& error)
    {
        return {.error = "Hokuto can't open " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

App::OpenWork App::ImportedArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes)
{
    // The new file with its timelines laid out for editing, and the archive for playback with the group files beside
    // it. The file keeps the group files, and the streams beside the archive that its file table names. The timelines
    // build their archive for playback.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        work.archive = song::LoadArchive(bytes, name, file);
        const ntdlib::sar::SoundArchive& read = *work.archive;
        work.document = std::make_shared<const format::File>(
            edit::OpenTimelines(format::ImportArchive(bytes, read.ReadOptions().flavor, read.GroupFiles(),
                                                      song::ReadStreams(read, file), read.GroupFileGroups())));
        if (!work.document->timelines->empty())
        {
            bytes = format::ExportArchive(*work.document);
            work.archive = song::LoadArchive(bytes, name, format::ExportGroupFiles(*work.document));
        }
        work.sequences = format::SequencesOfFiles(*work.document, bytes, *work.archive);
        work.start_fields = StartFieldsOf(*work.document, bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        return {.error = "Hokuto can't import " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::runtime_error& error)
    {
        return {.error = error.what()};
    }
    catch (const std::exception& error)
    {
        return {.error = "Hokuto can't import " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

App::OpenWork App::ImportedWiiArchive(const std::filesystem::path& file, std::vector<uint8_t> bytes)
{
    // The new file with its timelines laid out for editing, and the archive for the Wii's model. The file's export
    // gives the game's archive back, byte for byte. The game's main.dol on the extracted disc gives the build of NW4R
    // SND, and the file keeps it. Without it, the archive's version gives the build. The file keeps the streams beside
    // the archive that its file table names. The timelines build their archive for playback.
    const std::string name = xsflib::Utf8(file.filename());
    OpenWork work;
    work.imported = true;
    try
    {
        std::optional<ntdlib::SndBuild> build;
        if (const std::optional<std::filesystem::path> dol = song::FindMainDol(file))
        {
            std::vector<uint8_t> code;
            if (xsflib::ReadFile(*dol, code))
            {
                build = song::ReadWiiBuild(code);
            }
        }
        std::map<uint32_t, std::vector<uint8_t>> streams =
            song::ReadStreams(*song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions), file);
        work.document = std::make_shared<const format::File>(
            edit::OpenTimelines(format::ImportWiiArchive(bytes, build, std::move(streams))));
        if (!work.document->timelines->empty())
        {
            bytes = format::ExportArchive(*work.document);
        }
        work.wii = song::LoadWiiArchive(bytes, name, ntdlib::rvl::snd::kRsarOptions);
        work.sequences = format::SequencesOfFiles(*work.document, bytes, *work.wii);
        work.start_fields = StartFieldsOf(*work.document, bytes);
    }
    catch (const xsflib::FormatError& error)
    {
        return {.error = "Hokuto can't import " + name + ". " + format::AsSentence(error.what())};
    }
    catch (const std::exception& error)
    {
        return {.error = "Hokuto can't import " + name + ". " + format::AsSentence(error.what())};
    }

    return work;
}

void App::FinishOpening()
{
    // A cancelled open's work goes once it's done. The end of the work counts as an event.
    std::erase_if(dropped_opens_, Done<OpenWork>);
    if (!opening_ || !Done(opening_->work))
    {
        return;
    }

    OpenWork work = opening_->work.get();
    const std::string path = std::move(opening_->path);
    const std::string name = std::move(opening_->name);
    opening_.reset();
    Wake();
    if (!work.error.empty())
    {
        // A recent project that doesn't open leaves the list.
        const std::size_t recent = settings_.recent_projects.size();
        settings_.recent_projects = WithoutRecentProject(std::move(settings_.recent_projects), path);
        if (settings_.recent_projects.size() != recent)
        {
            ImGui::MarkIniSettingsDirty();
            work.error += " It leaves Recent Projects.";
        }
        ShowMessage(work.error);
        return;
    }

    // An imported sound archive gives a file that stays untitled. Its dialog starts beside the archive.
    ShowDocument(std::move(work.document), std::move(work.archive), std::move(work.sequences),
                 std::move(work.start_fields), name, std::move(work.wii));
    document_path_ = work.imported ? std::string() : path;
    import_path_ = work.imported ? path : std::string();
    text_form_ = work.text_form;
    if (!work.imported)
    {
        AddRecentProject(path);
    }
}

void App::AddRecentProject(const std::string& path)
{
    settings_.recent_projects = WithRecentProject(std::move(settings_.recent_projects), path);
    ImGui::MarkIniSettingsDirty();
}

void App::DrawOpening()
{
    if (!opening_)
    {
        if (ImGui::IsPopupOpen(kOpening) && ImGui::BeginPopupModal(kOpening))
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }

    if (!ImGui::IsPopupOpen(kOpening))
    {
        ImGui::OpenPopup(kOpening);
    }

    // A window over the middle of the main window, in the style of the Import window. The file's name is its title.
    const std::string title = opening_->name + kOpening;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Px(420.0f), 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(theme::kDialogPadding));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(theme::kRaised));
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme::kBorderStrong));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    const bool visible = ImGui::BeginPopupModal(title.c_str(), &open, kFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    bool cancel = !visible;
    if (visible)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kTextMuted), "Opening the file…");
        ImGui::SameLine();
        const float button = ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - button, 0.0f));
        cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (cancel)
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Cancel keeps the file that shows. The work runs on to its end.
    if (cancel)
    {
        dropped_opens_.push_back(std::move(opening_->work));
        opening_.reset();
    }
}

void App::NewDocument(format::Engine engine)
{
    // The new file, and the archive that it builds for playback. The archive holds its player alone.
    std::shared_ptr<const format::File> document;
    std::shared_ptr<const ntdlib::sar::SoundArchive> archive;
    std::vector<std::optional<uint32_t>> sequences;
    std::optional<std::vector<std::optional<uint32_t>>> start_fields;
    try
    {
        document = std::make_shared<const format::File>(format::NewFile(engine));
        const std::vector<uint8_t> built = format::ExportArchive(*document);
        archive = song::LoadArchive(built, kUntitled, format::ExportGroupFiles(*document));
        sequences = format::SequencesOfFiles(*document, built, *archive);
        start_fields = StartFieldsOf(*document, built);
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Hokuto can't start a new file. ") + format::AsSentence(error.what()));
        return;
    }

    ShowDocument(std::move(document), std::move(archive), std::move(sequences), std::move(start_fields), kUntitled);
    document_path_.clear();
    import_path_.clear();
    text_form_ = false;
}

void App::ShowDocument(std::shared_ptr<const format::File> document,
                       std::shared_ptr<const ntdlib::sar::SoundArchive> archive,
                       std::vector<std::optional<uint32_t>> sequences,
                       std::optional<std::vector<std::optional<uint32_t>>> start_fields, const std::string& name,
                       std::shared_ptr<const ntdlib::rsar::SoundArchive> wii)
{
    // The file replaces the one that was open before. The zones show no sequence until the user chooses one, and the
    // transport runs over the empty song. A preview from the last archive stops too, and so does the made-up song. An
    // export or a replacement in the last file ends. The editors of the last file's items close.
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    DropExport();
    replace_sound_.reset();
    replace_file_.reset();
    replace_wave_.reset();
    replace_stream_.reset();
    DropReplace();
    reader_.Abandon();
    StopPlayback();
    CancelCatchUp();
    worker_.Stop();
    history_.emplace(std::move(document));
    history_rows_.clear();
    action_selections_.clear();
    copied_track_.reset();
    preview_ = false;
    archive_ = std::move(archive);
    wii_archive_ = std::move(wii);
    archive_version_ = history_->Current();
    reading_version_.reset();
    file_sequences_ = std::move(sequences);
    start_fields_ = std::move(start_fields);
    start_values_.clear();
    told_missing_banks_.clear();
    changes_.clear();
    sequence_.reset();
    model_.reset();
    song_.reset();
    ForgetEngine();

    // The meters and the peak holds of the Stereo Out and the FX channels start again with the file. The tracks' go
    // with the song.
    master_.meter = {kSilence, kSilence};
    master_.peak = kSilence;
    fx_.meters.fill({kSilence, kSilence});
    fx_.peaks.fill(kSilence);
    effect_window_.reset();
    effect_draft_.reset();
    CloseKeyEditor();
    CloseListEditor();
    CloseBankEditor();
    bank_to_open_.reset();
    bank_drops_.clear();
    bank_drop_.reset();
    CloseProperties();
    file_name_ = name;
    UpdateTitle();
    transport_.SetCycle(false);
    transport_.SetPosition(0.0);
    transport_.TakeEmptySong();

    // The browser lists the archive's sounds, sequences, banks and wave archives from the top, with nothing chosen and
    // no search.
    ListBrowser();
    select_sound_.reset();
    browser_state_.filter.fill('\0');
    browser_state_.open.clear();
    browser_state_.selected = {};
    browser_state_.top = true;
    browser_state_.clicked = false;
    browser_state_.playing.reset();
    shown_group_ = -1;
    shown_item_ = -1;
    arrangement_.selected_track = 0;
    arrangement_.selected_part_track = -1;
    arrangement_.selected_part = -1;

    // The arrangement shows the new file from its start and its first track. A file that keeps a view scrolls to the
    // view's place once its sound shows (RestoreView).
    arrangement_.scroll_to = std::pair(0.0, 0.0f);

    // The view that a file keeps sets the shown sound, and the view waits for the sound to show.
    pending_view_ = history_->Current()->view;
    const std::vector<uint32_t>& sounds = browser_items_[kSequencesGroup];
    const auto found = pending_view_ ? std::ranges::find(sounds, pending_view_->sound) : sounds.end();
    if (found != sounds.end())
    {
        browser_state_.selected = {static_cast<int>(kSequencesGroup), static_cast<int>(found - sounds.begin())};
        browser_state_.reveal = true;
    }
    else
    {
        pending_view_.reset();
    }
}

void App::Save()
{
    if (document_path_.empty())
    {
        ShowDialog(Dialog::kSaveAs);
    }
    else
    {
        SaveTo(document_path_);
    }
}

void App::SaveTo(const std::string& path)
{
    if (!history_ || preview_)
    {
        return;
    }

    // The file's bytes in its form.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::string text;
    std::vector<uint8_t> binary;
    try
    {
        format::File saved = edit::FoldTimelines(*Document());
        saved.view = CurrentView();
        if (text_form_)
        {
            text = format::WriteText(saved);
        }
        else
        {
            binary = format::Write(saved);
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't save " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    const std::span<const uint8_t> bytes = text_form_
                                               ? std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())
                                               : std::span<const uint8_t>(binary);

    // The bytes go to a file beside the target first and then replace it. A save that fails partway leaves the last
    // save whole.
    std::filesystem::path saving = file;
    saving += ".saving";
    const bool written = !xsflib::WriteFile(saving, bytes);
    std::error_code moved;
    if (written)
    {
        std::filesystem::rename(saving, file, moved);
    }
    if (!written || moved)
    {
        std::error_code removed;
        std::filesystem::remove(saving, removed);
        ShowMessage("Hokuto can't save " + name +
                    ". The disk may be full, or the file or its folder may be read-only.");
        return;
    }

    // The saved file becomes the document's file, and the version that it holds the saved one.
    document_path_ = xsflib::Utf8(file);
    import_path_.clear();
    file_name_ = name;
    history_->MarkSaved();
    UpdateTitle();
    AddRecentProject(document_path_);
}

void App::AfterChanges(std::function<void()> then)
{
    // While the Save As dialog of an earlier request is open, a request does nothing.
    if (after_save_)
    {
        return;
    }

    if (!history_ || preview_ || !history_->Modified())
    {
        then();
        return;
    }

    // A save that fails shows its message, and the document stays.
    const SaveChoice choice = AskToSave();
    if (choice == SaveChoice::kDontSave)
    {
        then();
    }
    else if (choice == SaveChoice::kSave && document_path_.empty())
    {
        after_save_ = std::move(then);
        ShowDialog(Dialog::kSaveAs);
    }
    else if (choice == SaveChoice::kSave)
    {
        SaveTo(document_path_);
        if (!history_->Modified())
        {
            then();
        }
    }
}

App::SaveChoice App::AskToSave()
{
    // The question shows over the window. A minimized window comes back for it.
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        SDL_RestoreWindow(window_);
    }

    // Save answers Return, and Cancel answers Escape. In SDL3's right-to-left order, macOS puts Save on the right,
    // Cancel beside it and Don't Save on the left. The system's dialogs order them that way. Windows reads its buttons
    // the other way: Save on the left, Don't Save beside it and Cancel on the right. Programs on Windows ask in that
    // order. A box that can't be shown counts as Cancel.
#ifdef _WIN32
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel"},
                                                {0, 0, "Don't Save"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Save"}};
#else
    const SDL_MessageBoxButtonData buttons[] = {{0, 0, "Don't Save"},
                                                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 1, "Cancel"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Save"}};
#endif
    const std::string message =
        "Do you want to save the changes to " + file_name_ + "? Your changes will be lost if you don't save them.";
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT;
    box.window = window_;
    box.title = "Hokuto";
    box.message = message.c_str();
    box.numbuttons = 3;
    box.buttons = buttons;
    int chosen = 1;
    if (!SDL_ShowMessageBox(&box, &chosen))
    {
        SDL_Log("The question about the changes can't be shown: %s", SDL_GetError());
        return SaveChoice::kCancel;
    }

    if (chosen == 0)
    {
        return SaveChoice::kDontSave;
    }

    return chosen == 2 ? SaveChoice::kSave : SaveChoice::kCancel;
}

void App::StartExport(ExportTarget target)
{
    const format::File* document = Document();
    const bool from_sequence = target != ExportTarget::kSoundArchive;
    const bool midi = target == ExportTarget::kMidiSoundFont || target == ExportTarget::kMidiSfz;
    if (export_ || !history_ || preview_ || document == nullptr || !document->native ||
        (from_sequence && (!sequence_ || ((!archive_ && !wii_archive_) || !archive_version_))) ||
        (wii_archive_ && from_sequence && !midi && !SongEngineOf(target)))
    {
        return;
    }

    // The work exports the version that the history has reached.
    Export job;
    job.view = SongEngineOf(target) ? OtherConsoleExportView(document->engine, sequence_->name)
                                    : FileExportView(target, from_sequence ? sequence_->name : "");
    const std::shared_ptr<const format::File> version = history_->Current();
    if (!from_sequence)
    {
        // An imported archive keeps its file's name. The game's sizes measure the growth. Without them, the file as it
        // was opened measures it, and a version that builds the same archive didn't grow. The streams that the game
        // lacks go beside the archive. The archive's file table gives their places.
        const std::shared_ptr<const format::File> original = history_->Original();
        const std::filesystem::path source = xsflib::PathFromUtf8(import_path_.empty() ? document_path_ : import_path_);
        const char* extension = document->native->wii                                   ? ".brsar"
                                : document->native->flavor == ntdlib::sar::Flavor::kCtr ? ".bcsar"
                                                                                        : ".bfsar";
        job.file_name =
            import_path_.empty() ? xsflib::Utf8(source.stem()) + extension : xsflib::Utf8(source.filename());
        job.source_folder = source.parent_path();

        // A sound that plays on another console's engine in Hokuto goes out as the archive holds it. The file's tables
        // name it, or else the archive that the zones play.
        std::vector<std::string> on_engines;
        for (const auto& [sound, engine] : version->sound_engines)
        {
            if (engine == version->engine)
            {
                continue;
            }

            if (version->native->tables && sound < version->native->tables->sounds.size())
            {
                on_engines.push_back(version->native->tables->sounds[sound].name);
            }
            else if (wii_archive_ && sound < wii_archive_->Sounds().size())
            {
                on_engines.push_back(wii_archive_->Sounds()[sound].name);
            }
            else if (archive_ && sound < archive_->Sounds().size())
            {
                on_engines.push_back(archive_->Sounds()[sound].name);
            }
        }
        std::vector<std::string> notes;
        if (!on_engines.empty())
        {
            notes.push_back(EngineSoundsNote(
                on_engines, document->native->wii ? std::string("Wii") : song::ConsoleName(document->native->flavor)));
        }
        const auto build = [version, original, notes, source_folder = job.source_folder]
        {
            // The streams that the game lacks go beside the archive. The file table of the exported archive, `opened`,
            // gives their places.
            ExportWork work;
            std::vector<uint32_t> streams;
            for (const auto& [id, stream] : version->native->streams)
            {
                if (!stream.from_game)
                {
                    streams.push_back(id);
                }
            }
            const auto add_streams = [&work, &version, &streams](const auto& opened)
            {
                const std::vector<std::filesystem::path> paths = song::StreamPaths(opened, streams);
                std::vector<std::string> names;
                for (std::size_t i = 0; i < streams.size(); i++)
                {
                    names.push_back(xsflib::Utf8(paths[i]));
                    work.files.emplace_back(names.back(), *version->native->streams.at(streams[i]).bytes);
                }
                work.warnings.push_back(StreamsNote(names));
            };
            try
            {
                // An archive of the Wii keeps its groups inside. The game's size measures its growth.
                if (version->native->wii)
                {
                    work.archive = format::ExportArchive(edit::FoldTimelines(*version));
                    if (version->native->game_sizes)
                    {
                        work.warnings = GrowthWarnings({version->native->game_sizes->archive, work.archive.size(), {}});
                    }
                    if (!streams.empty())
                    {
                        add_streams(*song::LoadWiiArchive(work.archive, "", ntdlib::rvl::snd::kRsarOptions));
                    }
                    work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
                    return work;
                }

                const ntdlib::sar::Flavor flavor = version->native->flavor;
                const format::File folded = edit::FoldTimelines(*version);
                work.archive = format::ExportArchive(folded);
                std::optional<format::GameSizes> before = version->native->game_sizes;
                if (!before && !SameArchive(*version, *original))
                {
                    const format::File folded_original = edit::FoldTimelines(*original);
                    before = format::SizesOf(format::ExportArchive(folded_original), flavor,
                                             format::GroupFilesBeside(folded_original));
                }
                if (before)
                {
                    work.warnings = GrowthWarnings(
                        format::MeasureGrowth(*before, work.archive, flavor, format::GroupFilesBeside(folded)));
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

                    const std::string group =
                        changed.name.empty() ? "GROUP_" + std::to_string(*changed.group) : changed.name;
                    group_names.push_back(ntdlib::sar::GroupFileName(seqlib::SafeFileName(group), flavor));
                    work.group_files.push_back(std::move(changed));
                }
                if (!group_names.empty())
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
                    work.warnings.push_back(GroupFilesNote(placed));
                }

                if (!streams.empty())
                {
                    add_streams(*song::LoadArchive(work.archive, "", format::ExportGroupFiles(folded)));
                }

                work.warnings.insert(work.warnings.end(), notes.begin(), notes.end());
            }
            catch (const std::exception& error)
            {
                work.error = std::string("Hokuto can't export the sound archive. ") + format::AsSentence(error.what());
            }

            return work;
        };
        job.work = std::async(std::launch::async, build);
    }
    else if (SongEngineOf(target))
    {
        StartSongWork(job);
    }
    else
    {
        StartMidiWork(job, sequence_->sound);
    }

    export_ = std::move(job);
}

void App::StartMidiWork(Export& job, uint32_t sound)
{
    // The archive that the zones play serves when it came from the version and holds no timelines. Otherwise the work
    // first builds the version's archive. A saved file holds the same archive. A sound on another console's engine goes
    // out as the conversion for that console. A saved file holds the same conversion. Hokuto's warnings come before the
    // converter's. The MIDI file takes the tags.
    const std::shared_ptr<const format::File> version = history_->Current();
    const ExportTarget target = job.Target();
    const bool shown = sequence_ && sequence_->sound == sound;
    job.file_name =
        seqlib::SafeFileName(wii_archive_ ? wii_archive_->Sounds().at(sound).name : archive_->Sounds().at(sound).name) +
        ".mid";
    const bool same = SameArchive(*version, *archive_version_) && !(version->timelines && !version->timelines->empty());
    const format::Engine engine = format::EngineOf(*version, sound);
    const bool converts = Converts(*version, sound);
    std::vector<std::string> warnings =
        shown ? MidiWarnings(*sequence_, version->aux_effects, version->aux_volumes, target)
              : std::vector<std::string>();
    const auto convert = [version, archive = same && !converts ? archive_ : nullptr,
                          wii = same && !converts ? wii_archive_ : nullptr, name = file_name_, sound, shown, converts,
                          engine, target, values = StartValuesOf(sound), changes = ChangesOf(sound),
                          warnings = std::move(warnings), tags = MidiTagsOf(*version, sound)]() mutable
    {
        ExportWork work;
        try
        {
            // The conversion's file holds the sound alone. A conversion for the Wii converts by the rules of its file's
            // build of NW4R SND, and a file of the Wii plays by the rules of the game's build.
            std::vector<std::string> notes;
            const std::optional<ntdlib::SndBuild> build =
                version->native->wii ? std::optional(format::BuildOfWiiArchive(*version->native)) : std::nullopt;
            if (converts)
            {
                edit::OtherConsoleSong song = edit::ForEngine(*version, sound, engine);
                const std::vector<uint8_t> built = format::ExportArchive(edit::FoldTimelines(song.file));
                if (engine == format::Engine::kWii)
                {
                    const std::optional<ntdlib::SndBuild> converted = format::BuildOfWiiArchive(*song.file.native);
                    work.played_wii = song::LoadWiiArchive(built, name, ntdlib::rvl::snd::kRsarOptions);
                    work.conversion = song::ConvertToMidi(*work.played_wii, 0, values, changes, "Hokuto", converted);
                    if (!shown)
                    {
                        work.played_sequence = std::make_shared<const song::Sequence>(
                            song::ReadSequence(*work.played_wii, 0, values, changes, converted));
                    }
                }
                else
                {
                    work.played = song::LoadArchive(built, name);
                    work.conversion = song::ConvertToMidi(*work.played, 0, values, changes, "Hokuto");
                    if (!shown)
                    {
                        work.played_sequence = std::make_shared<const song::Sequence>(
                            song::ReadSequence(*work.played, 0, values, changes));
                    }
                }
                notes = std::move(song.warnings);
            }
            else if (version->native->wii)
            {
                if (!wii)
                {
                    wii = song::LoadWiiArchive(format::ExportArchive(edit::FoldTimelines(*version)), name,
                                               ntdlib::rvl::snd::kRsarOptions);
                }
                work.conversion = song::ConvertToMidi(*wii, sound, values, changes, "Hokuto", build);
            }
            else
            {
                if (!archive)
                {
                    const format::File folded = edit::FoldTimelines(*version);
                    archive = song::LoadArchive(format::ExportArchive(folded), name, format::ExportGroupFiles(folded));
                }
                work.conversion = song::ConvertToMidi(*archive, sound, values, changes, "Hokuto");
            }

            // A sound that the zones don't show gives the warnings from its sequence in the file's archive. Without a
            // conversion for another engine, the measure plays that sequence.
            if (!shown)
            {
                if (!archive && !wii)
                {
                    const format::File folded = edit::FoldTimelines(*version);
                    if (version->native->wii)
                    {
                        wii = song::LoadWiiArchive(format::ExportArchive(folded), name, ntdlib::rvl::snd::kRsarOptions);
                    }
                    else
                    {
                        archive =
                            song::LoadArchive(format::ExportArchive(folded), name, format::ExportGroupFiles(folded));
                    }
                }
                work.sequence =
                    std::make_shared<const song::Sequence>(wii ? song::ReadSequence(*wii, sound, values, changes, build)
                                                               : song::ReadSequence(*archive, sound, values, changes));
                warnings = MidiWarnings(*work.sequence, version->aux_effects, version->aux_volumes, target);
                if (!converts)
                {
                    work.played = archive;
                    work.played_wii = wii;
                    work.played_sequence = work.sequence;
                }
            }
            if (converts)
            {
                warnings.insert(warnings.begin(), EngineNote(EngineName(engine), EngineName(version->engine)));
            }

            work.conversion->midi = song::WithMidiTags(work.conversion->midi, tags);
            const std::vector<std::string> report =
                ConverterWarnings(song::ConversionReport(*work.conversion), target == ExportTarget::kMidiSfz);
            work.warnings = std::move(warnings);
            work.warnings.insert(work.warnings.begin() + (converts ? 1 : 0), notes.begin(), notes.end());
            work.warnings.insert(work.warnings.end(), report.begin(), report.end());
        }
        catch (const std::exception& error)
        {
            work.error = error.what();
        }

        return work;
    };
    job.work = std::async(std::launch::async, convert);
}

void App::StartSongWork(Export& job)
{
    // The work sends the shown sequence's sound of the version into a new file of the target's console
    // (edit::ForOtherConsole, or edit::ForWii for the Wii). The sound is the one that a saved file holds. The new
    // file's archive comes along for the measure.
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
            work.error = "Hokuto can't send the sound to " + named + ". " + format::AsSentence(error.what());
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

    // The work's files and warnings, once it's done. A window with other targets shows an error in place of the
    // warnings, and the user can choose another target. A window with one target closes with a message. A song for the
    // 3DS whose sound plays stereo waves offers them as mono. The check box shows what the work did.
    if (export_ && !export_->done && Done(export_->work))
    {
        export_->done = export_->work.get();
        if (!export_->done->error.empty() && export_->view.lines.size() > 1)
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
    }

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
    measuring.status = "Measuring how the export sounds…";
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
        // The work and the measure for the old target or option go. A song's target starts its measure again once the
        // new work is done. Another target starts without the option. The option sends a song to the 3DS again, with
        // its stereo waves in stereo or in mono.
        export_->view.refused.clear();
        if (choice == ExportChoice::kTarget)
        {
            export_->view.option.clear();
            export_->mono = edit::MonoWaves::kAuto;
        }
        else
        {
            export_->mono = export_->view.option_on ? edit::MonoWaves::kOn : edit::MonoWaves::kOff;
        }
        if (!export_->done)
        {
            dropped_exports_.push_back(std::move(export_->work));
        }
        export_->done.reset();
        if (export_->measure.valid())
        {
            export_->cancel_measure->store(true);
            dropped_measures_.push_back(std::move(export_->measure));
        }
        export_->measured.reset();
        if (export_->item)
        {
            StartItemWork(*export_);
        }
        else
        {
            StartSongWork(*export_);
        }
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

void App::DropExport()
{
    if (export_ && !export_->done)
    {
        dropped_exports_.push_back(std::move(export_->work));
    }
    if (export_ && export_->measure.valid())
    {
        export_->cancel_measure->store(true);
        dropped_measures_.push_back(std::move(export_->measure));
    }
    export_.reset();
}

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
    // render on the Wii's model.
    const std::shared_ptr<const ntdlib::sar::SoundArchive> played = other ? job.done->played : PlayedArchive();
    const std::shared_ptr<const ntdlib::rsar::SoundArchive> played_wii =
        other ? job.done->played_wii : PlayedWiiArchive();
    const std::shared_ptr<const song::Sequence> played_sequence = other ? job.done->played_sequence
                                                                  : engine_sound_ && engine_sequence_ ? engine_sequence_
                                                                                                      : sequence_;
    const song::Mixing mixing = other && document != nullptr ? PlayedMixing(*document, *sound) : ShownMixing();
    const std::optional<format::Engine> to = SongEngineOf(job.Target());
    if (to)
    {
        const auto work = [target = job.done->archive, archive = played, wii = played_wii, sequence = played_sequence,
                           mixing, random, to = *to, model = std::string("The ") + EngineName(*to) + "'s model",
                           cancel = job.cancel_measure]() mutable
        {
            MeasuredView view;
            if ((!archive && !wii) || !sequence)
            {
                view.status = "Hokuto couldn't measure the export: no sequence plays.";
                return view;
            }

            try
            {
                // The new file holds one sound. A file of the Wii plays by the rules of the build that its archive's
                // version gives. The file that edit::ForWii makes plays by the same rules.
                measure::Renders renders;
                if (to == format::Engine::kWii)
                {
                    const std::shared_ptr<const ntdlib::rsar::SoundArchive> target_archive =
                        song::LoadWiiArchive(std::move(target), sequence->name, ntdlib::rvl::snd::kRsarOptions);
                    const auto target_sequence =
                        std::make_shared<const song::Sequence>(song::ReadSequence(*target_archive, 0));
                    renders = wii ? measure::RenderOtherConsole(wii, sequence, target_archive, target_sequence, *cancel)
                                  : measure::RenderOtherConsole(archive, sequence, target_archive, target_sequence,
                                                                mixing, *cancel);
                }
                else
                {
                    const std::shared_ptr<const ntdlib::sar::SoundArchive> target_archive =
                        song::LoadArchive(std::move(target), sequence->name);
                    const auto target_sequence =
                        std::make_shared<const song::Sequence>(song::ReadSequence(*target_archive, 0));
                    // The new file plays in strict mixing where its console takes no other (format::MixingRefused).
                    const song::Mixing target_mixing =
                        format::MixingRefused(to, format::Mixing::kEnhanced) ? song::Mixing::kStrict : mixing;
                    renders = wii ? measure::RenderOtherConsole(wii, sequence, target_archive, target_sequence,
                                                                target_mixing, *cancel)
                                  : measure::RenderOtherConsole(archive, sequence, target_archive, target_sequence,
                                                                mixing, target_mixing, *cancel);
                }
                if (cancel->load())
                {
                    return view;
                }

                const measure::Measure found = measure::MeasureRenders(renders);
                view.lines = MeasuredLines(found.comparison, model, "", found.seconds, found.cut, random);
                for (const measure::PlaceClips& place : found.places)
                {
                    view.places.push_back({PlaceLine(place.place), place.export_clip, place.hokuto_clip, found.rate});
                }
            }
            catch (const std::exception& error)
            {
                view.status = std::string("Hokuto couldn't measure the export. ") + format::AsSentence(error.what());
            }

            return view;
        };
        job.measure = std::async(std::launch::async, work);
        return;
    }

    const auto instruments =
        job.Target() == ExportTarget::kMidiSoundFont ? measure::Instruments::kSoundFont : measure::Instruments::kSfz;
    const auto work = [conversion = *job.done->conversion, instruments, archive = played, wii = played_wii,
                       sequence = played_sequence, mixing, random, cancel = job.cancel_measure]()
    {
        MeasuredView view;
        if ((!archive && !wii) || !sequence)
        {
            view.status = "Hokuto couldn't measure the export: no sequence plays.";
            return view;
        }

        try
        {
            const measure::Measure found =
                wii ? measure::MeasureExport(conversion, instruments, wii, sequence, *cancel)
                    : measure::MeasureExport(conversion, instruments, archive, sequence, mixing, *cancel);
            const bool sound_font = instruments == measure::Instruments::kSoundFont;
            view.lines = MeasuredLines(
                found.comparison, sound_font ? "Hokuto's SoundFont 2 renderer" : "sfizz 1.2.3",
                sound_font ? "The renderer follows the SoundFont 2.04 specification where the common players do. "
                             "Where they depart from it, it takes their conventions. One is 0.4 of each zone's "
                             "attenuation."
                           : "",
                found.seconds, found.cut, random);
            view.closing = "Other players can play the files otherwise.";
            for (const measure::PlaceClips& place : found.places)
            {
                view.places.push_back({PlaceLine(place.place), place.export_clip, place.hokuto_clip, found.rate});
            }
        }
        catch (const std::exception& error)
        {
            view.status = std::string("Hokuto couldn't measure the export. ") + format::AsSentence(error.what());
        }

        return view;
    };
    job.measure = std::async(std::launch::async, work);
}

void App::StartReplace(const BrowserPlace& place)
{
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const bool item =
        place.group >= 0 && place.item >= 0 && group < browser_items_.size() && index < browser_items_[group].size();
    const auto child = static_cast<std::size_t>(place.child);
    const bool listed = item && place.child >= 0 && child < browser_children_[group][index].size();
    const bool wave = listed && group == kWavesGroup;
    const bool wave_sound_child = listed && group == kWaveSoundFilesGroup;
    if (!item || (place.child >= 0 && !wave && !wave_sound_child) || export_ || replace_ || !history_ || preview_ ||
        (!archive_ && !wii_archive_))
    {
        return;
    }
    if (wii_archive_)
    {
        StartWiiReplace(place);
        return;
    }

    // A sequence sound takes a sequence file. A bank, a wave archive or a wave sound takes a file of its kind, and a
    // wave takes a WAV file. A stream sound takes a stream file or a WAV file. A wave sound under its wave sound file
    // takes a wave sound file. So does a wave sound in the sounds' list. The file comes from the dialog. The program
    // reads it in a later frame.
    const uint32_t number = wave_sound_child ? browser_children_[group][index][child] : browser_items_[group][index];
    const bool wave_sound = group == kSoundsGroup && number < archive_->Sounds().size() &&
                            archive_->Sounds()[number].type == ntdlib::sar::SoundType::kWaveSound;
    const bool stream = group == kSoundsGroup && song::StreamFileOf(*archive_, number).has_value();
    replace_sound_.reset();
    replace_file_.reset();
    replace_wave_.reset();
    replace_stream_.reset();
    if (wave)
    {
        replace_wave_ = WaveTarget{number, browser_children_[group][index][child]};
    }
    else if (group == kSequencesGroup && start_fields_)
    {
        replace_sound_ = number;
    }
    else if (group == kBanksGroup)
    {
        replace_file_ = FileTarget{edit::FileKind::kBank, number};
    }
    else if (group == kWavesGroup)
    {
        replace_file_ = FileTarget{edit::FileKind::kWaveArchive, number};
    }
    else if (wave_sound || group == kWaveSoundFilesGroup)
    {
        replace_file_ = FileTarget{edit::FileKind::kWaveSoundFile, number};
    }
    else if (stream)
    {
        replace_stream_ = number;
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

void App::StartWiiReplace(const BrowserPlace& place)
{
    // On the Wii, a sequence sound takes a sequence file of the Wii, and a bank or a wave sound a file of its kind. A
    // wave archive is the wave data of a bank or a wave sound file. It takes an RWAR where the file keeps its waves in
    // one. A wave of the wave data takes a WAV file or a wave file of the Wii. A stream sound takes a stream file of
    // the Wii, the 3DS, the Wii U or the Switch, or a WAV file. The file comes from the dialog. The program reads it in
    // a later frame.
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    const bool wave_sound_child = place.child >= 0 && group == kWaveSoundFilesGroup;
    const uint32_t number = wave_sound_child ? browser_children_[group][index][static_cast<std::size_t>(place.child)]
                                             : browser_items_[group][index];
    const bool wave_sound = group == kSoundsGroup && number < wii_archive_->Sounds().size() &&
                            wii_archive_->Sounds()[number].type == ntdlib::rsar::SoundType::kWaveSound;
    const bool stream = group == kSoundsGroup && song::StreamFileOf(*wii_archive_, number).has_value();
    replace_sound_.reset();
    replace_file_.reset();
    replace_wave_.reset();
    replace_stream_.reset();
    if (place.child >= 0 && group == kWavesGroup)
    {
        replace_wave_ = WaveTarget{number, browser_children_[group][index][static_cast<std::size_t>(place.child)]};
        ShowDialog(Dialog::kReplace);
        return;
    }
    if (place.child >= 0 && !wave_sound_child)
    {
        return;
    }

    if (group == kSequencesGroup && start_fields_)
    {
        replace_sound_ = number;
    }
    else if (group == kBanksGroup)
    {
        replace_file_ = FileTarget{edit::FileKind::kBank, number};
    }
    else if (group == kWavesGroup)
    {
        replace_file_ = FileTarget{edit::FileKind::kWaveArchive, number};
    }
    else if (wave_sound || group == kWaveSoundFilesGroup)
    {
        replace_file_ = FileTarget{edit::FileKind::kWaveSoundFile, number};
    }
    else if (stream)
    {
        replace_stream_ = number;
    }
    else
    {
        return;
    }

    ShowDialog(Dialog::kReplace);
}

void App::OpenReplacement(const std::string& path)
{
    if (replace_file_)
    {
        OpenFileReplacement(path);
        return;
    }
    if (replace_wave_)
    {
        OpenWaveReplacement(path);
        return;
    }
    if (replace_stream_)
    {
        OpenStreamReplacement(path);
        return;
    }

    const std::optional<uint32_t> sound = std::exchange(replace_sound_, std::nullopt);
    if (!sound || !history_ || (!archive_ && !wii_archive_) || !start_fields_)
    {
        return;
    }

    // The file, read as the replacement reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    // A Wii archive takes a sequence file of the Wii, read by the rules of the archive's build of NW4R SND. A file of
    // another console takes the biquad filters that come nearest on the archive's console.
    ReplaceSource source;
    const std::shared_ptr<const format::File> current = history_->Current();
    try
    {
        source.incoming = edit::ReadIncoming(bytes, edit::ConsoleOf(*current));
    }
    catch (const std::invalid_argument& error)
    {
        const std::string sound_name =
            wii_archive_ ? wii_archive_->Sounds()[*sound].name : archive_->Sounds()[*sound].name;
        ShowMessage("Hokuto can't put " + name + " in the place of " + sound_name + "'s sequence. " +
                    format::AsSentence(error.what()));
        return;
    }

    // The Replace window starts from the version that the history has reached.
    source.file = current;
    source.archive = archive_;
    source.wii = wii_archive_;
    source.places = wii_archive_ ? edit::SoundPlaces(*wii_archive_, file_sequences_, *start_fields_)
                                 : edit::SoundPlaces(*archive_, file_sequences_, *start_fields_);
    source.sound = *sound;
    source.file_name = name;
    FindSoundStarts(source);
    FindReaders(source);
    FindGroups(source);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    replacement.view = MakeReplaceView(replacement.source, std::nullopt, {}, {}, false);
    replace_ = std::move(replacement);
}

void App::OpenFileReplacement(const std::string& path)
{
    const std::optional<FileTarget> target = std::exchange(replace_file_, std::nullopt);
    if (!target || !history_ || (!archive_ && !wii_archive_))
    {
        return;
    }
    if (wii_archive_)
    {
        OpenWiiFileReplacement(path, *target);
        return;
    }

    // The file, read as the replacement reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::vector<int> targets;
    try
    {
        const ntdlib::sar::Flavor flavor = archive_->ReadOptions().flavor;
        incoming = edit::ReadIncomingFile(bytes, target->kind, flavor,
                                          edit::IncomingVersions(*archive_, target->kind, target->item));
        if (target->kind != edit::FileKind::kWaveArchive)
        {
            // The new file's wave archives keep their numbers where the present file names them too.
            named = edit::WaveArchivesNamed(incoming.bytes, flavor);
            std::vector<edit::NamedWaveArchive> present;
            try
            {
                present = edit::WaveArchivesNamed(
                    archive_->FileData(edit::FileIdOf(*archive_, target->kind, target->item)), flavor);
            }
            catch (const std::invalid_argument&)
            {
                // A present file that ntdlib can't read names no wave archive.
            }
            targets = FirstWaveArchives(*archive_, incoming.bytes, named, present);
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " +
                    ReplacedName(*archive_, target->kind, target->item) + ". " + format::AsSentence(error.what()));
        return;
    }

    replace_ = FileReplacement(std::move(incoming), name, *target, std::move(named), targets);
}

void App::OpenWiiFileReplacement(const std::string& path, const FileTarget& target)
{
    // The file, read as the replacement reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    const std::string item = ReplacedName(*wii_archive_, target.kind, target.item);

    // A bank file or a wave sound file of another console goes in converted, with the wave data chosen in the Replace
    // window. Its rows start at the item's wave data.
    const std::optional<ntdlib::sar::Flavor> later =
        target.kind == edit::FileKind::kWaveArchive ? std::nullopt : edit::LaterConsoleOf(bytes, target.kind);
    if (later)
    {
        edit::IncomingFile incoming;
        std::vector<edit::NamedWaveArchive> named;
        std::vector<int> targets;
        try
        {
            incoming = edit::ReadIncomingFile(bytes, target.kind, *later);
            named = edit::WaveArchivesNamed(incoming.bytes, *later);
            ReplaceSource first;
            first.wii = wii_archive_;
            first.later = later;
            targets = FirstWiiWaveData(first, incoming, named, edit::FileIdOf(*wii_archive_, target.kind, target.item));
        }
        catch (const std::exception& error)
        {
            ShowMessage("Hokuto can't put " + name + " in the place of " + item + ". " +
                        format::AsSentence(error.what()));
            return;
        }

        replace_ = WiiFileReplacement(std::move(incoming), name, target, *later, std::move(named), targets);
        return;
    }

    edit::IncomingFile incoming;
    edit::FileChanges changes;
    ReplaceSource source;
    source.file = history_->Current();
    source.wii = wii_archive_;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*wii_archive_, file_sequences_, *start_fields_);
    }
    source.sound = target.item;
    source.file_name = name;
    try
    {
        incoming = edit::ReadWiiIncomingFile(bytes, target.kind);
        changes = edit::ChangesOf(*source.file, *wii_archive_, target.item, incoming);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + item + ". " + format::AsSentence(error.what()));
        return;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::ReplaceWiiFile(*source.file, *wii_archive_, target.item, incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, target.kind, std::nullopt, {}, std::move(incoming), {}};
    replacement.view =
        MakeWiiFileReplaceView(replacement.source, *replacement.new_item, changes, std::move(result), std::move(error));
    replace_ = std::move(replacement);
}

std::optional<App::Replacement> App::WiiFileReplacement(edit::IncomingFile incoming, const std::string& name,
                                                        const FileTarget& target, ntdlib::sar::Flavor later,
                                                        std::vector<edit::NamedWaveArchive> named,
                                                        const std::vector<int>& targets)
{
    // The file in the present file's version with the chosen wave data, and what it changes. The edit starts from the
    // version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source;
    source.file = history_->Current();
    source.wii = wii_archive_;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*wii_archive_, file_sequences_, *start_fields_);
    }
    source.sound = target.item;
    source.file_name = name;
    source.later = later;
    edit::IncomingFile mapped = incoming;
    edit::FileChanges changes;
    std::optional<format::File> result;
    std::string error;
    try
    {
        const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*wii_archive_);
        std::map<uint32_t, uint32_t> map;
        for (std::size_t k = 0; k < named.size(); k++)
        {
            const int each = k < targets.size() ? targets[k] : -1;
            if (each < 0 || static_cast<std::size_t>(each) >= wave_data.size())
            {
                throw std::invalid_argument(
                    "Choose wave data of the archive for each wave archive that the file names.");
            }

            map[named[k].id] = wave_data[static_cast<std::size_t>(each)].file_id;
        }
        mapped = edit::WiiFileOf(incoming, later, *wii_archive_, map,
                                 edit::PresentWiiVersion(*wii_archive_, target.kind, target.item));
        changes = edit::ChangesOf(*source.file, *wii_archive_, target.item, mapped);
    }
    catch (const std::exception& failure)
    {
        mapped = incoming;
        error = failure.what();
    }
    if (error.empty())
    {
        try
        {
            result = edit::ReplaceWiiFile(*source.file, *wii_archive_, target.item, mapped);
        }
        catch (const std::invalid_argument& failure)
        {
            error = failure.what();
        }
    }
    Replacement replacement{std::move(source), {}, target.kind, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view = MakeWiiFileReplaceView(replacement.source, mapped, changes, std::move(result), std::move(error),
                                              replacement.named, targets);

    return replacement;
}

std::optional<App::Replacement> App::FileReplacement(edit::IncomingFile incoming, const std::string& name,
                                                     const FileTarget& target,
                                                     std::vector<edit::NamedWaveArchive> named,
                                                     const std::vector<int>& targets,
                                                     const std::vector<edit::ProgramFix>& fixes,
                                                     std::optional<std::vector<uint32_t>> joined,
                                                     std::vector<std::string> new_groups)
{
    // The file with the chosen wave archives, and what it changes. A bank then takes the chosen instruments. The edit
    // starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source;
    source.file = history_->Current();
    source.archive = archive_;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*archive_, file_sequences_, *start_fields_);
    }
    source.sound = target.item;
    source.file_name = name;
    FindGroups(source);
    source.joined = std::move(joined);
    source.new_groups = std::move(new_groups);
    edit::IncomingFile mapped = incoming;
    const bool chosen = std::ranges::none_of(targets, [](int each) { return each < 0; });
    std::optional<format::File> result;
    std::string error;
    edit::FileChanges changes;
    try
    {
        if (!named.empty() && chosen)
        {
            std::map<uint32_t, uint32_t> map;
            for (std::size_t k = 0; k < named.size() && k < targets.size(); k++)
            {
                map[named[k].id] = static_cast<uint32_t>(targets[k]);
            }
            mapped.bytes = edit::WithWaveArchiveMap(*source.file, incoming.bytes, map);
        }
        changes = edit::ChangesOf(*source.file, *archive_, target.item, mapped);
    }
    catch (const std::exception& failure)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " +
                    ReplacedName(*archive_, target.kind, target.item) + ". " + format::AsSentence(failure.what()));
        return std::nullopt;
    }

    try
    {
        if (target.kind == edit::FileKind::kBank && !fixes.empty())
        {
            mapped.bytes = edit::WithProgramFixes(*archive_, target.item, mapped.bytes, fixes);
        }
        if (chosen)
        {
            result = edit::ReplaceFile(*source.file, edit::FileIdOf(*archive_, target.kind, target.item), mapped.bytes);
        }
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, target.kind, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view = MakeFileReplaceView(replacement.source, mapped, changes, std::move(result), std::move(error),
                                           replacement.named, targets, fixes);

    return replacement;
}

void App::OpenWaveReplacement(const std::string& path)
{
    const std::optional<WaveTarget> target = std::exchange(replace_wave_, std::nullopt);
    if (!target || !history_ || (!archive_ && !wii_archive_))
    {
        return;
    }

    // A WAV file goes in as DSP-ADPCM until the user chooses another encoding in the Replace window.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    replace_ = wii_archive_ ? WiiWaveReplacement(std::move(bytes), name, *target, ntdlib::sar::WaveEncoding::kDspAdpcm)
                            : WaveReplacement(std::move(bytes), name, *target, ntdlib::sar::WaveEncoding::kDspAdpcm);
}

std::optional<App::Replacement> App::WaveReplacement(std::vector<uint8_t> wav, const std::string& name,
                                                     const WaveTarget& target, ntdlib::sar::WaveEncoding encoding,
                                                     std::optional<uint32_t> rate, std::optional<edit::WaveLoop> loop)
{
    // The wave of the WAV file or the wave file, the wave archive that it makes, and what the two change.
    const std::string wave_name = song::ItemName(*archive_, {song::ItemKind::kWave, target.wave_archive, target.wave});
    const bool wave_file = edit::IsWaveFile(wav);
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    edit::IncomingFile wave_archive;
    edit::FileChanges archive_changes;
    try
    {
        incoming = wave_file
                       ? edit::ReadIncomingWaveFile(wav, *archive_, target.wave_archive, target.wave)
                       : edit::ReadIncomingWave(wav, *archive_, target.wave_archive, target.wave, encoding, loop, rate);
        changes = edit::ChangesOfWave(*archive_, target.wave_archive, target.wave);
        wave_archive.kind = edit::FileKind::kWaveArchive;
        wave_archive.bytes = edit::WaveArchiveWith(*archive_, target.wave_archive, target.wave, incoming.bytes);
        archive_changes = edit::ChangesOf(*history_->Current(), *archive_, target.wave_archive, wave_archive);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + wave_name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source;
    source.file = history_->Current();
    source.archive = archive_;
    source.sound = target.wave_archive;
    source.file_name = name;
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::WithIncomingWave(*source.file, *archive_, target.wave_archive, target.wave, incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, target.wave, std::move(wav), std::nullopt, {}};
    replacement.view =
        MakeWaveReplaceView(replacement.source, target.wave, incoming, !wave_file, changes, archive_changes,
                            wave_archive.bytes.size(), std::move(result), std::move(error));

    return replacement;
}

std::optional<App::Replacement> App::WiiWaveReplacement(std::vector<uint8_t> wav, const std::string& name,
                                                        const WaveTarget& target, ntdlib::sar::WaveEncoding encoding,
                                                        std::optional<uint32_t> rate,
                                                        std::optional<edit::WaveLoop> loop)
{
    // The wave of the WAV file or the wave file, and the present wave and what plays it. A WAV file's wave takes the
    // present wave's version.
    const std::string wave_name =
        song::ItemName(*wii_archive_, {song::ItemKind::kWave, target.wave_archive, target.wave});
    const bool wave_file = edit::IsWiiWaveFile(wav) || edit::IsWaveFile(wav);
    edit::IncomingWave incoming;
    edit::WaveChanges changes;
    try
    {
        changes = edit::ChangesOfWiiWave(*wii_archive_, target.wave_archive, target.wave);
        const uint16_t version = edit::PresentWave(*wii_archive_, target.wave_archive, target.wave).version;
        incoming = wave_file ? edit::ReadIncomingWiiWaveFile(wav)
                             : edit::ReadIncomingWiiWave(wav, version, encoding, loop, rate);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + wave_name + ". " +
                    format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached. The Replace window says why it can't be made.
    ReplaceSource source;
    source.file = history_->Current();
    source.wii = wii_archive_;
    source.sound = target.wave_archive;
    source.file_name = name;
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::WithIncomingWiiWave(*source.file, *wii_archive_, target.wave_archive, target.wave, incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, target.wave, std::move(wav), std::nullopt, {}};
    replacement.view = MakeWiiWaveReplaceView(replacement.source, target.wave, incoming, !wave_file, changes,
                                              std::move(result), std::move(error));

    return replacement;
}

void App::OpenStreamReplacement(const std::string& path)
{
    const std::optional<uint32_t> sound = std::exchange(replace_stream_, std::nullopt);
    if (!sound || !history_ || (!archive_ && !wii_archive_))
    {
        return;
    }

    // A WAV file goes in as DSP-ADPCM until the user chooses another encoding in the Replace window.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    replace_ = StreamReplacement(std::move(bytes), name, *sound, ntdlib::sar::WaveEncoding::kDspAdpcm);
}

std::optional<App::Replacement> App::StreamReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                       uint32_t sound, ntdlib::sar::WaveEncoding encoding)
{
    if (wii_archive_)
    {
        return WiiStreamReplacement(std::move(bytes), name, sound, encoding);
    }

    // A sound that plays an AAC stream takes an AAC stream.
    std::optional<edit::AacChanges> aac;
    try
    {
        aac = edit::ChangesOfAacStream(*history_->Current(), *archive_, sound);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + archive_->Sounds().at(sound).name +
                    "'s stream. " + format::AsSentence(error.what()));
        return std::nullopt;
    }
    if (aac)
    {
        return AacStreamReplacement(std::move(bytes), name, sound, *aac);
    }

    // The stream of the stream file or the WAV file, and the stream that it replaces.
    const bool stream_file = edit::IsStreamFile(bytes);
    const ntdlib::sar::Flavor flavor = archive_->ReadOptions().flavor;
    edit::IncomingStream incoming;
    edit::StreamChanges changes;
    std::optional<std::vector<ntdlib::sar::StreamFile::Track>> given;
    try
    {
        const uint32_t version = edit::IncomingStreamVersion(*history_->Current(), flavor);
        incoming = stream_file ? edit::ReadIncomingStreamFile(bytes, flavor, version)
                               : edit::ReadIncomingStreamWav(bytes, flavor, version, encoding);
        changes = edit::ChangesOfStream(*history_->Current(), *archive_, sound);
        given = edit::SoundTracksFor(incoming, changes);
        incoming = edit::WithSoundTracks(std::move(incoming), changes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + archive_->Sounds().at(sound).name +
                    "'s stream. " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.archive = archive_;
    source.sound = sound;
    source.file_name = name;
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::ReplaceStream(*source.file, changes.file, incoming.bytes);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = changes.file;
    replacement.view = MakeStreamReplaceView(replacement.source, incoming, !stream_file, changes, given,
                                             std::move(result), std::move(error));

    return replacement;
}

std::optional<App::Replacement> App::AacStreamReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                          uint32_t sound, const edit::AacChanges& changes)
{
    // The system's encoder makes the new stream, unless an AAC file of the present rate and channels came in.
    edit::IncomingAac incoming;
    try
    {
        incoming = edit::ReadIncomingAac(bytes, changes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + archive_->Sounds().at(sound).name +
                    "'s stream. " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.archive = archive_;
    source.sound = sound;
    source.file_name = name;
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::ReplaceAacStream(*source.file, changes, incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = changes.file;
    replacement.view =
        MakeAacStreamReplaceView(replacement.source, incoming, changes, std::move(result), std::move(error));

    // Play plays the new stream decoded, with its loop.
    try
    {
        replacement.view.wave = song::PlayableStream(incoming.bytes, song::AacStream{incoming.loop});
    }
    catch (const std::exception&)
    {
        replacement.view.wave.clear();
    }

    return replacement;
}

bool App::PlaysAacStream(uint32_t sound) const
{
    if (!archive_ || !history_ || !history_->Current()->native)
    {
        return false;
    }

    const std::optional<uint32_t> stream = song::StreamFileOf(*archive_, sound);
    const auto& streams = history_->Current()->native->streams;
    const auto found = stream ? streams.find(*stream) : streams.end();
    return stream && song::AacStreamOf(*archive_, sound) &&
           (found == streams.end() || song::IsAdts(*found->second.bytes));
}

std::optional<App::Replacement> App::WiiStreamReplacement(std::vector<uint8_t> bytes, const std::string& name,
                                                          uint32_t sound, ntdlib::sar::WaveEncoding encoding)
{
    // A stream file of the Wii goes in as it is, and one of a later console converted. A WAV file goes in as a stream
    // that NW4R's tools would lay out. Either of the last two takes the present stream's tracks where they fit.
    const bool stream_file = edit::IsWiiStreamFile(bytes) || edit::IsStreamFile(bytes);
    edit::IncomingStream incoming;
    edit::StreamChanges changes;
    try
    {
        changes = edit::ChangesOfWiiStream(*history_->Current(), *wii_archive_, sound);
        incoming = stream_file ? edit::ReadIncomingWiiStreamFile(bytes, changes.wii_tracks)
                               : edit::ReadIncomingWiiStreamWav(bytes, encoding, changes.wii_tracks);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't put " + name + " in the place of " + wii_archive_->Sounds().at(sound).name +
                    "'s stream. " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.wii = wii_archive_;
    source.sound = sound;
    source.file_name = name;
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::ReplaceStream(*source.file, changes.file, incoming.bytes);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.stream = changes.file;
    replacement.view = MakeWiiStreamReplaceView(replacement.source, incoming, !stream_file, changes, std::move(result),
                                                std::move(error));

    return replacement;
}

void App::DrawReplace()
{
    // A dropped import's or preview's work goes once it's done. Once its work is done, a preview plays if the window
    // that started it is still open.
    std::erase_if(dropped_edits_, Done<EditWork>);
    std::erase_if(dropped_previews_, Done<PreviewWork>);
    if (preview_work_ && !replace_)
    {
        dropped_previews_.push_back(std::move(*preview_work_));
        preview_work_.reset();
    }
    else if (preview_work_ && Done(*preview_work_))
    {
        PreviewWork work = preview_work_->get();
        preview_work_.reset();
        replace_->view.preparing = false;
        Wake();
        if (work.renderer)
        {
            PlayReplacePreview(std::move(work.renderer));
        }
        else if (!work.note.empty())
        {
            ShowMessage(work.note, MessageKind::kNote);
        }
        else
        {
            ShowMessage("Hokuto can't play the mapping. " + format::AsSentence(work.error));
        }
    }

    // A confirmed import's version goes into the history once its work is done, and the browser then selects its new
    // sound. The window stays open with the reason when the edit fails. The end of the work counts as an event: the
    // window draws on while the browser and the zones follow the new version.
    if (replace_ && replace_->pending && Done(replace_->pending->work))
    {
        EditWork work = replace_->pending->work.get();
        PendingEdit pending = std::move(*replace_->pending);
        replace_->pending.reset();
        Wake();
        if (work.result)
        {
            if (pending.select)
            {
                select_sound_ = pending.select;
            }
            bank_to_open_ = pending.open_bank;
            replace_.reset();
            Do(std::move(pending.action), std::move(pending.details), std::move(*work.result), std::nullopt,
               std::move(pending.replaced));
        }
        else
        {
            ReplaceView& view = replace_->view;
            view.error = std::move(work.error);
            view.warnings.clear();
            view.ready = false;
            view.working.clear();
        }
    }

    // The window's Play ends as the window closes: after Cancel, after the edit, and when another file opens.
    if (!replace_)
    {
        if (replace_previewing_)
        {
            StopPreview();
        }
        CloseReplaceWindow();
        return;
    }

    // A preview that has played to its end no longer shows as playing, and Play reads Play again.
    if (replace_previewing_ && !worker_.Playing() && ring_.Available() == 0)
    {
        replace_previewing_ = false;
    }
    replace_->view.previewing = replace_previewing_;

    // A change of the start, of a bank slot, of a program, of the tracks or of a wave's encoding makes the view again.
    // Replace makes the edit. While an import's work runs, the window takes only Cancel. Stop ends the preview.
    const ReplaceChoice choice = DrawReplaceWindow(replace_->view, replace_place_);
    if (choice == ReplaceChoice::kStop)
    {
        StopPreview();
        return;
    }

    if (preview_work_ && choice != ReplaceChoice::kNone && choice != ReplaceChoice::kPlay)
    {
        dropped_previews_.push_back(std::move(*preview_work_));
        preview_work_.reset();
        replace_->view.preparing = false;
    }
    if (choice == ReplaceChoice::kCancel)
    {
        DropReplace();
        return;
    }

    if (std::exchange(replace_->view.groups_changed, false))
    {
        replace_->source.joined = replace_->view.joined;
        replace_->source.new_groups = replace_->view.new_groups;
    }
    if (replace_->pending)
    {
        return;
    }

    if (choice == ReplaceChoice::kPlay && wii_archive_ && (replace_->stream || replace_->incoming_stream))
    {
        // On the Wii, the new stream plays on NW4R SND's player of streams with the file's build. A new sound plays it
        // with the entry that the import gives it.
        try
        {
            const ntdlib::SndBuild build = format::BuildOfWiiArchive(*replace_->source.file->native);
            if (!replace_->incoming_stream)
            {
                PlayReplacePreview(std::make_unique<song::SoundPreview>(wii_archive_, replace_->source.sound,
                                                                        replace_->view.wave, build));
            }
            else if (replace_->new_stream && replace_->new_stream->wii_sound)
            {
                PlayReplacePreview(std::make_unique<song::SoundPreview>(wii_archive_, *replace_->new_stream->wii_sound,
                                                                        replace_->view.wave, build));
            }
            else
            {
                throw std::runtime_error(replace_->view.error);
            }
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Hokuto can't play the new stream. ") + format::AsSentence(error.what()));
        }
    }
    else if (choice == ReplaceChoice::kPlay && archive_ && (replace_->stream || replace_->incoming_stream))
    {
        // The new stream plays on the console's model with the sound's tracks. A new sound plays it with the model's
        // volume.
        try
        {
            ntdlib::sar::SoundInfo sound = archive_->Sounds().at(replace_->source.sound);
            if (replace_->incoming_stream)
            {
                sound.stream = replace_->new_stream ? replace_->new_stream->details : ntdlib::sar::StreamSoundInfo();
            }
            const format::Mixing mixing = format::MixingOf(*replace_->source.file, replace_->source.sound);
            PlayReplacePreview(std::make_unique<song::SoundPreview>(archive_, std::move(sound), replace_->view.wave,
                                                                    edit::ModelMixing(mixing)));
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Hokuto can't play the new stream. ") + format::AsSentence(error.what()));
        }
    }
    else if (choice == ReplaceChoice::kChange && replace_->incoming_stream)
    {
        // Another encoding makes the stream again. Another name or model keeps it.
        const ReplaceView& view = replace_->view;
        const uint32_t model =
            view.model < replace_->source.models.size() ? replace_->source.models[view.model] : replace_->source.sound;
        const ntdlib::sar::WaveEncoding encoding = view.encoding.value_or(replace_->incoming_stream->encoding);
        std::optional<edit::IncomingStream> incoming = std::move(replace_->incoming_stream);
        if (encoding != incoming->encoding)
        {
            incoming.reset();
        }
        const std::string file_name = replace_->source.file_name;
        replace_ = replace_->source.wii ? WiiStreamImport(std::move(replace_->wav), file_name, model, NameOf(view),
                                                          encoding, std::move(incoming))
                                        : StreamImport(std::move(replace_->wav), file_name, model, NameOf(view),
                                                       encoding, std::move(incoming));
    }
    else if (choice == ReplaceChoice::kReplace && replace_->incoming_stream && replace_->view.ready &&
             replace_->new_stream)
    {
        MakeStreamImport();
    }
    else if (choice == ReplaceChoice::kPlay && replace_->view.playable && replace_->view.wave.empty())
    {
        PlayMapping();
    }
    else if (choice == ReplaceChoice::kPlay && wii_archive_ &&
             (!replace_->view.wave.empty() || !replace_->view.waves.empty()))
    {
        // A wave of the Wii plays as the Wii decodes it: its first two channels, up to the end of its loop. A new wave
        // archive plays the wave chosen in the menu.
        try
        {
            PlayReplacePreview(std::make_unique<song::WavePreview>(
                edit::WavePcm(ntdlib::rsar::WaveFile::Read(ChosenWave(replace_->view)))));
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Hokuto can't play the new wave. ") + format::AsSentence(error.what()));
        }
    }
    else if (choice == ReplaceChoice::kPlay && archive_)
    {
        // The new wave plays by itself. Playback follows the console's decoding: the wave's first two channels, up to
        // the end of its loop. A new wave archive plays the wave chosen in the menu.
        try
        {
            ntdlib::sar::Pcm pcm = ntdlib::sar::DecodeWave(ChosenWave(replace_->view), song::ItemOptions(*archive_));
            pcm.channels.resize(std::min<std::size_t>(pcm.channels.size(), 2));
            PlayReplacePreview(std::make_unique<song::WavePreview>(std::move(pcm)));
        }
        catch (const std::exception& error)
        {
            ShowMessage(std::string("Hokuto can't play the new wave. ") + format::AsSentence(error.what()));
        }
    }
    else if (choice == ReplaceChoice::kChange && replace_->new_wave)
    {
        // Another wave archive, encoding or rate makes the new wave again. On the Wii, the menu lists the wave data of
        // the archive's files.
        const ReplaceView& view = replace_->view;
        const auto wave_archive = static_cast<uint32_t>(std::max(view.wave_archive.value_or(0), 0));
        const ntdlib::sar::WaveEncoding encoding = view.encoding.value_or(ntdlib::sar::WaveEncoding::kDspAdpcm);
        const bool wav = view.encoding.has_value();
        const std::optional<uint32_t> rate = ChosenRate(view);
        const std::string name = replace_->source.file_name;
        if (replace_->source.wii)
        {
            const std::vector<song::WiiWaveArchive> wave_data = song::WaveArchivesOf(*replace_->source.wii);
            const uint32_t file_id =
                wave_archive < wave_data.size() ? wave_data[wave_archive].file_id : replace_->source.sound;
            replace_ = WiiWaveImport(std::move(replace_->wav), name, file_id, encoding, wav, rate);
        }
        else
        {
            replace_ = WaveImport(std::move(replace_->wav), name, wave_archive, encoding, wav, rate);
        }
    }
    else if (choice == ReplaceChoice::kReplace && replace_->new_wave && replace_->view.result)
    {
        const ReplaceSource& source = replace_->source;
        std::string action = replace_->view.encoding ? "Import Audio File" : "Import Wave";
        const song::Item wave{song::ItemKind::kWave, source.sound, replace_->wave.value_or(0)};
        std::string details = (source.wii ? song::ItemName(*source.wii, wave) : song::ItemName(*source.archive, wave)) +
                              " from " + source.file_name;
        format::File result = std::move(*replace_->view.result);
        replace_.reset();
        Do(std::move(action), std::move(details), std::move(result));
    }
    else if (choice == ReplaceChoice::kChange && replace_->view.import && replace_->new_item)
    {
        const ReplaceView& view = replace_->view;
        if (view.model < replace_->source.models.size())
        {
            replace_->source.sound = replace_->source.models[view.model];
        }
        replace_->view = replace_->source.wii
                             ? MakeWiiItemImportView(replace_->source, *replace_->new_item, replace_->named,
                                                     ChosenWaveArchives(view), NameOf(view))
                             : MakeItemImportView(replace_->source, *replace_->new_item, replace_->named,
                                                  ChosenWaveArchives(view), NameOf(view));
    }
    else if (choice == ReplaceChoice::kChange && replace_->view.import)
    {
        // The slots follow the model until the user chooses a slot's bank.
        const ReplaceView& view = replace_->view;
        if (view.model < replace_->source.models.size())
        {
            replace_->source.sound = replace_->source.models[view.model];
        }
        if (view.slots_changed)
        {
            replace_->source.banks = ChosenBanks(view);
        }

        // An empty song's labels take the new sound's name, and the starts show them.
        if (replace_->source.new_song)
        {
            replace_->source.incoming.sequence = edit::EmptySong(NameOf(view));
        }
        replace_->view = replace_->source.wii ? MakeWiiImportView(replace_->source, view.chosen, ChosenPrograms(view),
                                                                  ChosenVariables(view), NameOf(view))
                                              : MakeImportView(replace_->source, view.chosen, ChosenPrograms(view),
                                                               ChosenVariables(view), NameOf(view));
    }
    else if (choice == ReplaceChoice::kReplace && replace_->view.import && replace_->view.ready && replace_->new_item)
    {
        MakeItemImport();
    }
    else if (choice == ReplaceChoice::kReplace && replace_->view.import && replace_->view.ready)
    {
        MakeImport();
    }
    else if (choice == ReplaceChoice::kChange && replace_->stream && replace_->view.encoding)
    {
        const uint32_t sound = replace_->source.sound;
        const std::string name = replace_->source.file_name;
        replace_ = StreamReplacement(std::move(replace_->wav), name, sound, *replace_->view.encoding);
    }
    else if (choice == ReplaceChoice::kReplace && replace_->stream && replace_->view.result)
    {
        const ReplaceSource& source = replace_->source;
        std::string details =
            (source.wii ? source.wii->Sounds()[source.sound].name : source.archive->Sounds()[source.sound].name) +
            " from " + source.file_name;
        format::File result = std::move(*replace_->view.result);
        replace_.reset();
        Do("Replace Stream", std::move(details), std::move(result));
    }
    else if (choice == ReplaceChoice::kChange && replace_->file && replace_->new_item && !replace_->source.wii)
    {
        // Another wave archive or instrument in a row makes the file again. The sound that Play plays stays, and so do
        // the groups that the user chose. The Replace window of a file of the Wii has no rows.
        const FileTarget target{*replace_->file, replace_->source.sound};
        const std::string name = replace_->source.file_name;
        const std::vector<int> targets = ChosenWaveArchives(replace_->view);
        const std::vector<edit::ProgramFix> fixes = ChosenFixes(replace_->view);
        const std::size_t play = replace_->view.play;
        replace_ = FileReplacement(std::move(*replace_->new_item), name, target, std::move(replace_->named), targets,
                                   fixes, std::move(replace_->source.joined), std::move(replace_->source.new_groups));
        if (replace_ && play < replace_->view.play_sounds.size())
        {
            replace_->view.play = play;
        }
    }
    else if (choice == ReplaceChoice::kChange && replace_->file && replace_->new_item && replace_->source.wii &&
             replace_->source.later)
    {
        // Other wave data in a row converts the file again.
        const FileTarget target{*replace_->file, replace_->source.sound};
        const std::string name = replace_->source.file_name;
        const ntdlib::sar::Flavor later = *replace_->source.later;
        const std::vector<int> targets = ChosenWaveArchives(replace_->view);
        replace_ = WiiFileReplacement(std::move(*replace_->new_item), name, target, later, std::move(replace_->named),
                                      targets);
    }
    else if (choice == ReplaceChoice::kChange && replace_->wave && replace_->view.encoding)
    {
        const WaveTarget target{replace_->source.sound, *replace_->wave};
        const std::string name = replace_->source.file_name;
        replace_ = replace_->source.wii
                       ? WiiWaveReplacement(std::move(replace_->wav), name, target, *replace_->view.encoding,
                                            ChosenRate(replace_->view), ChosenLoop(replace_->view))
                       : WaveReplacement(std::move(replace_->wav), name, target, *replace_->view.encoding,
                                         ChosenRate(replace_->view), ChosenLoop(replace_->view));
    }
    else if (choice == ReplaceChoice::kChange)
    {
        // The slots stay the sound's until the user chooses a slot's bank.
        const ReplaceView& view = replace_->view;
        if (view.slots_changed)
        {
            replace_->source.banks = ChosenBanks(view);
        }
        replace_->view = MakeReplaceView(replace_->source, view.chosen, ChosenPrograms(view), ChosenVariables(view),
                                         view.all_tracks);
    }
    else if (choice == ReplaceChoice::kReplace && replace_->view.result)
    {
        const ReplaceSource& source = replace_->source;
        const std::optional<edit::FileKind> kind = replace_->file;
        const std::optional<uint32_t> wave = replace_->wave;
        std::string action = wave ? "Replace Wave" : kind ? ReplaceAction(*kind) : "Replace Sequence";
        const std::string item = source.wii
                                     ? (wave ? song::ItemName(*source.wii, {song::ItemKind::kWave, source.sound, *wave})
                                        : kind ? ReplacedName(*source.wii, *kind, source.sound)
                                               : source.wii->Sounds()[source.sound].name)
                                 : wave ? song::ItemName(*source.archive, {song::ItemKind::kWave, source.sound, *wave})
                                 : kind ? ReplacedName(*source.archive, *kind, source.sound)
                                        : source.archive->Sounds()[source.sound].name;
        std::string details = item + " from " + source.file_name;
        std::vector<uint32_t> replaced = !wave && !kind ? ReplacedSequences(source) : std::vector<uint32_t>{};
        format::File result = std::move(*replace_->view.result);
        if (!replace_->view.joined.empty() || !replace_->view.new_groups.empty())
        {
            // The groups take the files that the item requires after the replacement and didn't require before. The new
            // groups take them all.
            const uint32_t id =
                kind ? ReplacedItem(*kind, source.sound) : ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound);
            const auto make = [file = std::move(result), groups = replace_->view.joined,
                               new_groups = replace_->view.new_groups, id,
                               before = edit::FilesOfItem(*source.archive, id)]
            {
                return edit::JoinGroups(file, groups, std::span(&id, 1), before, new_groups);
            };
            StartEdit(make, std::move(action), std::move(details), std::nullopt, std::move(replaced));
            return;
        }

        replace_.reset();
        Do(std::move(action), std::move(details), std::move(result), std::nullopt, std::move(replaced));
    }
}

void App::DropReplace()
{
    if (replace_ && replace_->pending)
    {
        dropped_edits_.push_back(std::move(replace_->pending->work));
    }
    replace_.reset();
}

void App::StartSequenceImport()
{
    // The new sound copies the settings of a sequence sound. Without one, a file in the model form and a file of the
    // Wii give it Hokuto's settings. An archive in the game's layout can't take it, and the import says so before the
    // dialog.
    if (!IsEnabled(Command::kImportSequence))
    {
        return;
    }
    if (archive_ && history_->Current()->form != format::ArchiveForm::kModel &&
        !HoldsSoundOf(*archive_, {ntdlib::sar::SoundType::kSequence}))
    {
        ShowMessage(
            "Hokuto can't import a sequence here yet. The new sound copies the settings of a sequence sound, "
            "and the archive holds none.");
        return;
    }

    ShowDialog(Dialog::kImportSequence);
}

void App::OpenSequenceImport(const std::string& path)
{
    if (!IsEnabled(Command::kImportSequence))
    {
        return;
    }

    // The file, read as the import reads it.
    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    // A file of the Wii takes a sequence file by the rules of its build of NW4R SND, and converts one of another
    // console. A file of another console takes the biquad filters that come nearest on the archive's console.
    ReplaceSource source;
    try
    {
        source.incoming = edit::ReadIncoming(bytes, edit::ConsoleOf(*history_->Current()));
    }
    catch (const std::invalid_argument& error)
    {
        ShowMessage("Hokuto can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    OpenSequenceSource(std::move(source), name, "Hokuto can't import " + name + ".");
}

void App::StartSoundAddition()
{
    if (!IsEnabled(Command::kAddSequenceSound))
    {
        return;
    }

    ReplaceSource source;
    source.incoming.flavor = archive_ ? archive_->ReadOptions().flavor : ntdlib::sar::Flavor::kNx;
    source.incoming.wii = wii_archive_ != nullptr;
    source.incoming.sequence = edit::EmptySong(kNewSoundName);
    source.new_song = true;
    OpenSequenceSource(std::move(source), kNewSoundName, "Hokuto can't add a sound.");
}

void App::OpenSequenceSource(ReplaceSource source, const std::string& name, const std::string& what)
{
    if (wii_archive_)
    {
        OpenWiiSequenceSource(std::move(source), name);
        return;
    }

    // The new sound copies the settings of the sequence sound that the zones show, or else of the archive's first
    // sequence sound. In a file in the model form without one, it takes Hokuto's settings, and its one bank slot the
    // archive's first bank.
    for (std::size_t i = 0; i < archive_->Sounds().size(); i++)
    {
        if (archive_->Sounds()[i].type == ntdlib::sar::SoundType::kSequence)
        {
            source.models.push_back(static_cast<uint32_t>(i));
        }
    }
    if (source.models.empty() && history_->Current()->form != format::ArchiveForm::kModel)
    {
        ShowMessage(what + " The archive holds no sequence sound for the new one to copy.");
        return;
    }

    if (source.models.empty())
    {
        source.sound = ntdlib::sar::kNoItem;
        source.banks = std::vector<uint32_t>{
            archive_->Banks().empty() ? ntdlib::sar::kNoItem : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, 0)};
    }
    else
    {
        const bool shown = sequence_ && std::ranges::find(source.models, sequence_->sound) != source.models.end();
        source.sound = shown ? sequence_->sound : source.models.front();
    }

    // The Import window starts from the version that the history has reached. Where the archive's sounds play gives the
    // window their starts in a file that holds their commands.
    source.file = history_->Current();
    source.archive = archive_;
    source.file_name = name;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*archive_, file_sequences_, *start_fields_);
    }
    FindSoundStarts(source);
    FindReaders(source);
    FindGroups(source);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    replacement.view =
        MakeImportView(replacement.source, std::nullopt, {}, {}, FirstName(*replacement.source.file, *archive_, name));
    replace_ = std::move(replacement);
}

void App::OpenWiiSequenceSource(ReplaceSource source, const std::string& name)
{
    // The new sound copies the settings of the sequence sound that the zones show, or else of the archive's first
    // sequence sound. Without one, it takes Hokuto's settings, and its bank slot the archive's first bank.
    for (std::size_t i = 0; i < wii_archive_->Sounds().size(); i++)
    {
        if (wii_archive_->Sounds()[i].type == ntdlib::rsar::SoundType::kSequence)
        {
            source.models.push_back(static_cast<uint32_t>(i));
        }
    }
    if (source.models.empty())
    {
        source.sound = ntdlib::sar::kNoItem;
        source.banks = std::vector<uint32_t>{
            wii_archive_->Banks().empty() ? ntdlib::sar::kNoItem : ntdlib::sar::ItemId(ntdlib::sar::kBankItem, 0)};
    }
    else
    {
        const bool shown = sequence_ && std::ranges::find(source.models, sequence_->sound) != source.models.end();
        source.sound = shown ? sequence_->sound : source.models.front();
    }

    // The Import window starts from the version that the history has reached. Where the archive's sounds play gives the
    // window their starts in a file that holds their commands. The new sound joins groups with names. An archive that
    // NW4R's tools didn't lay out takes no new sound, and the window says so from the start.
    source.file = history_->Current();
    source.wii = wii_archive_;
    source.file_name = name;
    if (start_fields_)
    {
        source.places = edit::SoundPlaces(*wii_archive_, file_sequences_, *start_fields_);
    }
    FindSoundStarts(source);
    FindReaders(source);
    FindGroups(source);
    source.refused = edit::WiiAdditionsRefused(*source.file);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::nullopt, {}};
    replacement.view =
        MakeWiiImportView(replacement.source, std::nullopt, {}, {}, FirstWiiName(replacement.source, name));
    replace_ = std::move(replacement);
}

void App::MakeWiiImport()
{
    // The edit builds the archive again with the new sound. The browser then selects the new sound. An empty song takes
    // the sound's name. A converted sequence gives the global variables the later consoles' starting value. The Import
    // window showed that value.
    const ReplaceSource& source = replace_->source;
    const ReplaceView& view = replace_->view;
    const std::string name = NameOf(view);
    const std::optional<uint32_t> model = source.models.empty() ? std::nullopt : std::optional<uint32_t>(source.sound);
    const auto added = static_cast<uint32_t>(source.wii->Sounds().size());
    const auto make =
        [file = source.file, model, name, sequence = source.new_song ? edit::EmptySong(name) : source.incoming.sequence,
         start = view.starts[view.chosen].item, programs = ChosenPrograms(view), variables = ChosenVariables(view),
         banks = source.banks, groups = view.joined, converted = source.incoming.converted]
    {
        edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
        if (converted)
        {
            mapped.start = edit::WithLaterGlobals(mapped.sequence, mapped.start).first;
        }

        return edit::AddWiiSequenceSound(*file, model, name, mapped.sequence, mapped.start, programs, banks, groups);
    };
    if (source.new_song)
    {
        StartEdit(make, "Add Sequence Sound", name, added);
    }
    else
    {
        StartEdit(make, "Import Sequence", name + " from " + source.file_name, added);
    }
}

void App::MakeImport()
{
    if (wii_archive_)
    {
        MakeWiiImport();
        return;
    }

    // The edit builds the archive again with the new sound. The browser then selects the new sound. An empty song takes
    // the sound's name.
    const ReplaceSource& source = replace_->source;
    const ReplaceView& view = replace_->view;
    const std::string name = NameOf(view);
    const std::optional<uint32_t> model = source.models.empty() ? std::nullopt : std::optional<uint32_t>(source.sound);

    // The new sound joins the chosen groups. A group that loads the model takes only the files that the model doesn't
    // require (edit::JoinGroups).
    const auto added = static_cast<uint32_t>(source.file->native->tables->sounds.size());
    const auto make =
        [file = source.file, model, name, sequence = source.new_song ? edit::EmptySong(name) : source.incoming.sequence,
         start = view.starts[view.chosen].item, programs = ChosenPrograms(view), variables = ChosenVariables(view),
         banks = source.banks, groups = view.joined, new_groups = view.new_groups, added,
         before = model ? edit::FilesOfItem(*source.archive, ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, *model))
                        : std::vector<uint32_t>()]
    {
        const edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
        const uint32_t item = ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, added);
        return edit::JoinGroups(
            edit::AddSequenceSound(*file, model, name, mapped.sequence, mapped.start, programs, banks), groups,
            std::span(&item, 1), before, new_groups);
    };
    if (source.new_song)
    {
        StartEdit(make, "Add Sequence Sound", name, added);
    }
    else
    {
        StartEdit(make, "Import Sequence", name + " from " + source.file_name, added);
    }
}

void App::OpenItemImport(const std::string& path, edit::FileKind kind)
{
    const Command command = kind == edit::FileKind::kBank          ? Command::kImportBank
                            : kind == edit::FileKind::kWaveArchive ? Command::kImportWaveArchive
                                                                   : Command::kImportWaveSoundFile;
    if (!IsEnabled(command))
    {
        return;
    }

    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    // On the Wii, a bank file or a wave sound file can have its wave data beside it in an RWAR of the same name.
    // Export… writes the wave data that way.
    ReplaceSource source;
    if (wii_archive_ && kind != edit::FileKind::kWaveArchive)
    {
        std::filesystem::path beside = file;
        beside.replace_extension(".brwar");
        std::error_code error;
        if (std::filesystem::is_regular_file(beside, error) && xsflib::ReadFile(beside, source.beside))
        {
            source.beside_name = xsflib::Utf8(beside.filename());
        }
        else
        {
            source.beside.clear();
        }
    }
    OpenItemBytes(std::move(bytes), name, kind, std::move(source));
}

void App::StartBankAddition()
{
    if (!IsEnabled(Command::kAddBank))
    {
        return;
    }

    ReplaceSource source;
    source.new_bank = true;
    if (wii_archive_)
    {
        // An empty bank of the Wii comes with its wave data.
        ntdlib::rsar::BankFile::Files files = edit::EmptyBank(*wii_archive_);
        edit::IncomingFile incoming{edit::FileKind::kBank,
                                    edit::NewWiiFileVersion(*wii_archive_, edit::FileKind::kBank),
                                    std::move(files.file), std::nullopt, std::move(files.wave_data)};
        OpenWiiItem(std::move(incoming), {}, 1, std::string(kNewBankName) + ".brbnk", std::move(source));
        return;
    }

    OpenItemBytes(edit::EmptyBank(archive_->ReadOptions().flavor), std::string(kNewBankName) + ".bfbnk",
                  edit::FileKind::kBank, std::move(source));
}

void App::OpenItemBytes(std::vector<uint8_t> bytes, const std::string& name, edit::FileKind kind, ReplaceSource source)
{
    if (wii_archive_)
    {
        OpenWiiItemBytes(std::move(bytes), name, kind, std::move(source));
        return;
    }

    // The file, read as the import reads it.
    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::size_t sounds = 1;
    try
    {
        const ntdlib::sar::Flavor flavor = archive_->ReadOptions().flavor;
        incoming = edit::ReadIncomingFile(bytes, kind, flavor, edit::IncomingVersions(*archive_, kind));
        if (kind != edit::FileKind::kWaveArchive)
        {
            named = edit::WaveArchivesNamed(incoming.bytes, flavor);
        }
        if (kind == edit::FileKind::kWaveSoundFile)
        {
            sounds = std::max<std::size_t>(edit::WaveSoundCount(incoming.bytes, flavor), 1);
        }
    }
    catch (const std::invalid_argument& error)
    {
        ShowMessage("Hokuto can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    // New wave sounds copy the settings of a wave sound of the archive with the same file, or else of the first wave
    // sound, or else of the first sequence sound. The menu lists the wave sounds first.
    if (kind == edit::FileKind::kWaveSoundFile)
    {
        for (const ntdlib::sar::SoundType type :
             {ntdlib::sar::SoundType::kWaveSound, ntdlib::sar::SoundType::kSequence})
        {
            for (std::size_t i = 0; i < archive_->Sounds().size(); i++)
            {
                if (archive_->Sounds()[i].type == type)
                {
                    source.models.push_back(static_cast<uint32_t>(i));
                }
            }
        }
        if (source.models.empty())
        {
            ShowMessage("Hokuto can't import " + name +
                        ". The archive holds no wave sound or sequence sound for the new sounds to copy.");
            return;
        }

        const auto same_file = [this, &incoming](uint32_t sound)
        {
            const ntdlib::sar::SoundInfo& info = archive_->Sounds()[sound];
            return info.type == ntdlib::sar::SoundType::kWaveSound &&
                   std::ranges::equal(archive_->FileData(info.file_id), incoming.bytes);
        };
        const auto found = std::ranges::find_if(source.models, same_file);
        source.sound = found != source.models.end() ? *found : source.models.front();
    }

    // The Import window starts from the version that the history has reached.
    source.file = history_->Current();
    source.archive = archive_;
    source.file_name = name;
    FindGroups(source);
    const std::vector<int> targets = FirstWaveArchives(*archive_, incoming.bytes, named);
    const std::string first = FirstName(*source.file, *archive_, name, sounds);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view = MakeItemImportView(replacement.source, *replacement.new_item, replacement.named, targets, first);
    replace_ = std::move(replacement);
}

void App::OpenWiiItemBytes(std::vector<uint8_t> bytes, const std::string& name, edit::FileKind kind,
                           ReplaceSource source)
{
    // The file, read as the import reads it. A bank file or a wave sound file of another console waits for the Import
    // window's wave data. A wave archive of another console goes in converted.
    edit::IncomingFile incoming;
    std::vector<edit::NamedWaveArchive> named;
    std::size_t sounds = 1;
    try
    {
        const std::optional<ntdlib::sar::Flavor> later =
            kind == edit::FileKind::kWaveArchive ? std::nullopt : edit::LaterConsoleOf(bytes, kind);
        if (later)
        {
            incoming = edit::ReadIncomingFile(bytes, kind, *later);
            named = edit::WaveArchivesNamed(incoming.bytes, *later);
            source.later = later;
            source.beside.clear();
            source.beside_name.clear();
            if (kind == edit::FileKind::kWaveSoundFile)
            {
                sounds = std::max<std::size_t>(edit::WaveSoundCount(incoming.bytes, *later), 1);
            }
        }
        else
        {
            incoming = edit::ReadWiiIncomingFile(bytes, kind);
            if (kind == edit::FileKind::kWaveSoundFile)
            {
                sounds = std::max<std::size_t>(
                    ntdlib::rsar::WaveSoundFile::Parse(incoming.bytes, ntdlib::rvl::snd::kRsarOptions).Sounds().size(),
                    1);
            }
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't import " + name + ". " + format::AsSentence(error.what()));
        return;
    }

    OpenWiiItem(std::move(incoming), std::move(named), sounds, name, std::move(source));
}

void App::OpenWiiItem(edit::IncomingFile incoming, std::vector<edit::NamedWaveArchive> named, std::size_t sounds,
                      const std::string& name, ReplaceSource source)
{
    // New wave sounds copy the settings of a wave sound of the archive with the same file, or else of the first wave
    // sound, or else of the first sequence sound. The menu lists the wave sounds first.
    if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        for (const ntdlib::rsar::SoundType type :
             {ntdlib::rsar::SoundType::kWaveSound, ntdlib::rsar::SoundType::kSequence})
        {
            for (std::size_t i = 0; i < wii_archive_->Sounds().size(); i++)
            {
                if (wii_archive_->Sounds()[i].type == type)
                {
                    source.models.push_back(static_cast<uint32_t>(i));
                }
            }
        }
        if (source.models.empty())
        {
            ShowMessage("Hokuto can't import " + name +
                        ". The archive holds no wave sound or sequence sound for the new sounds to copy.");
            return;
        }

        const auto same_file = [this, &incoming](uint32_t sound)
        {
            const ntdlib::rsar::SoundInfo& info = wii_archive_->Sounds()[sound];
            return info.type == ntdlib::rsar::SoundType::kWaveSound &&
                   std::ranges::equal(wii_archive_->FileData(info.file_id), incoming.bytes);
        };
        const auto found = std::ranges::find_if(source.models, same_file);
        source.sound = found != source.models.end() ? *found : source.models.front();
    }

    // The Import window starts from the version that the history has reached. The new items join groups with names. An
    // archive that NW4R's tools didn't lay out takes no new item, and the window says so from the start.
    source.file = history_->Current();
    source.wii = wii_archive_;
    source.file_name = name;
    FindGroups(source);
    source.refused = edit::WiiAdditionsRefused(*source.file);
    const std::vector<int> targets = FirstWiiWaveData(source, incoming, named);
    const std::string first = FirstWiiName(source, name, sounds);
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, {}, std::move(incoming),
                            std::move(named)};
    replacement.view =
        MakeWiiItemImportView(replacement.source, *replacement.new_item, replacement.named, targets, first);
    replace_ = std::move(replacement);
}

void App::MakeWiiItemImport()
{
    // The work converts the file with the window's wave data, and builds the archive again with the new item. The
    // browser selects the first new sound once it lists the sound. The Bank Editor opens on a bank that Project > Add
    // Bank… adds.
    const ReplaceSource& source = replace_->source;
    const ReplaceView& view = replace_->view;
    const edit::IncomingFile& incoming = *replace_->new_item;
    const std::string name = NameOf(view);
    const bool wave_sounds = incoming.kind == edit::FileKind::kWaveSoundFile;
    auto make = [source, incoming, named = replace_->named, targets = ChosenWaveArchives(view), name,
                 groups = view.joined, wave_sounds]
    {
        const edit::IncomingFile imported = WiiImportedFile(source, incoming, named, targets);
        return wave_sounds ? edit::AddWiiWaveSounds(*source.file, source.sound, name, imported.bytes,
                                                    imported.wave_data, groups)
                           : edit::AddWiiBank(*source.file, name, imported.bytes, imported.wave_data, groups);
    };
    std::string action = source.new_bank                                 ? "Add Bank"
                         : incoming.kind == edit::FileKind::kBank        ? "Import Bank"
                         : incoming.kind == edit::FileKind::kWaveArchive ? "Import Wave Archive"
                                                                         : "Import Wave Sound File";
    std::optional<uint32_t> select;
    std::optional<uint32_t> open_bank;
    if (wave_sounds)
    {
        select = static_cast<uint32_t>(source.wii->Sounds().size());
    }
    else if (source.new_bank)
    {
        open_bank = static_cast<uint32_t>(source.wii->Banks().size());
    }
    std::string details = name + (source.new_bank ? std::string() : " from " + source.file_name);
    StartEdit(std::move(make), std::move(action), std::move(details), select);
    replace_->pending->open_bank = open_bank;
}

std::function<format::File()> App::ItemImportEdit(bool join) const
{
    // The edit builds the archive again with the new item.
    const ReplaceSource& source = replace_->source;
    const ReplaceView& view = replace_->view;
    std::map<uint32_t, uint32_t> map;
    const std::vector<int> targets = ChosenWaveArchives(view);
    for (std::size_t k = 0; k < replace_->named.size() && k < targets.size(); k++)
    {
        map[replace_->named[k].id] = static_cast<uint32_t>(targets[k]);
    }

    // With `join`, the new item, or each new wave sound, joins the chosen groups. A group that loads the sound whose
    // settings new wave sounds copy takes only the files that the sound doesn't require (edit::JoinGroups).
    const edit::IncomingFile& incoming = *replace_->new_item;
    std::vector<uint32_t> before;
    if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        before = edit::FilesOfItem(*source.archive, ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, source.sound));
    }

    return [file = source.file, sound = source.sound, name = NameOf(view), incoming, map,
            groups = join ? view.joined : std::vector<uint32_t>(),
            new_groups = join ? view.new_groups : std::vector<std::string>(), files_before = std::move(before)]
    {
        const format::NativeTables& tables = *file->native->tables;
        std::vector<uint32_t> items;
        format::File added;
        if (incoming.kind == edit::FileKind::kBank)
        {
            items.push_back(ntdlib::sar::ItemId(ntdlib::sar::kBankItem, static_cast<uint32_t>(tables.banks.size())));
            added = edit::AddBank(*file, name, incoming.bytes, map);
        }
        else if (incoming.kind == edit::FileKind::kWaveArchive)
        {
            items.push_back(
                ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, static_cast<uint32_t>(tables.wave_archives.size())));
            added = edit::AddWaveArchive(*file, name, incoming.bytes);
        }
        else
        {
            added = edit::AddWaveSounds(*file, sound, name, incoming.bytes, map);
            for (std::size_t s = tables.sounds.size(); s < added.native->tables->sounds.size(); s++)
            {
                items.push_back(ntdlib::sar::ItemId(ntdlib::sar::kSoundItem, static_cast<uint32_t>(s)));
            }
        }

        return edit::JoinGroups(added, groups, items, files_before, new_groups);
    };
}

void App::MakeItemImport()
{
    if (wii_archive_)
    {
        MakeWiiItemImport();
        return;
    }

    const ReplaceSource& source = replace_->source;
    const edit::IncomingFile& incoming = *replace_->new_item;
    const std::string name = NameOf(replace_->view);
    std::function<format::File()> make = ItemImportEdit(true);

    // The browser selects the first new sound once it lists the sound. The Bank Editor opens on a bank that Project >
    // Add Bank… adds.
    std::string action = "Import Wave Archive";
    std::optional<uint32_t> select;
    std::optional<uint32_t> open_bank;
    if (incoming.kind == edit::FileKind::kBank)
    {
        action = source.new_bank ? "Add Bank" : "Import Bank";
        if (source.new_bank)
        {
            open_bank = static_cast<uint32_t>(source.file->native->tables->banks.size());
        }
    }
    else if (incoming.kind == edit::FileKind::kWaveSoundFile)
    {
        action = "Import Wave Sound File";
        select = static_cast<uint32_t>(source.file->native->tables->sounds.size());
    }
    std::string details = (name.empty() ? std::string("A wave archive") : name) +
                          (source.new_bank ? std::string() : " from " + source.file_name);
    StartEdit(std::move(make), std::move(action), std::move(details), select);
    replace_->pending->open_bank = open_bank;
}

void App::StartEdit(std::function<format::File()> make, std::string action, std::string details,
                    std::optional<uint32_t> select, std::vector<uint32_t> replaced)
{
    // The work catches the edit's refusal. The window waits with the controls off, and Cancel drops the work.
    const auto work = [build = std::move(make)]
    {
        EditWork done;
        try
        {
            done.result = build();
        }
        catch (const std::exception& error)
        {
            done.error = error.what();
        }

        return done;
    };
    replace_->pending = PendingEdit{std::async(std::launch::async, work),
                                    std::move(action),
                                    std::move(details),
                                    select,
                                    std::move(replaced),
                                    std::nullopt};
    replace_->view.working = "Building the archive…";
}

void App::PlayMapping()
{
    // The version that the mapping makes, and what plays in it.
    const ReplaceSource& source = replace_->source;
    const ReplaceView& view = replace_->view;
    std::function<format::File()> make;
    PreviewTarget target;
    target.mixing = source.file->mixing;
    if (replace_->file && view.result)
    {
        // A file in the place of another plays the sound chosen in the menu, in its mixing mode. A bank or a wave
        // archive that no sound plays gives a note of its first instrument or its first wave.
        make = [result = *view.result]
        {
            return result;
        };
        if (view.play < view.play_sounds.size())
        {
            target.sound = view.play_sounds[view.play];
            target.mixing = format::MixingOf(*source.file, *target.sound);
        }
        else if (*replace_->file == edit::FileKind::kBank)
        {
            target.bank = source.sound;
        }
        else
        {
            target.wave_archive = source.sound;
        }
    }
    else if (view.import && view.ready && replace_->new_item)
    {
        // A new bank plays a note of its first instrument, and a new wave sound file its first wave sound.
        make = ItemImportEdit(false);
        if (replace_->new_item->kind == edit::FileKind::kBank)
        {
            target.bank = static_cast<uint32_t>(source.archive->Banks().size());
        }
        else
        {
            target.sound = static_cast<uint32_t>(source.archive->Sounds().size());
        }
    }
    else if (view.import && view.ready && view.chosen < view.starts.size() && source.models.empty())
    {
        // A new sequence sound without a model plays as the new sound.
        make = [file = source.file, name = NameOf(view), sequence = source.incoming.sequence,
                start = view.starts[view.chosen].item, programs = ChosenPrograms(view),
                variables = ChosenVariables(view), banks = source.banks]
        {
            const edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
            return edit::AddSequenceSound(*file, std::nullopt, name, mapped.sequence, mapped.start, programs, banks);
        };
        target.sound = static_cast<uint32_t>(source.archive->Sounds().size());
    }
    else if (view.import && view.ready && view.chosen < view.starts.size())
    {
        // A new sequence sound plays as its model would with the new commands. The edit puts them in the place of the
        // model's sequence. That builds the archive faster than a new sound.
        make = [file = source.file, places = source.places, sound = source.sound, sequence = source.incoming.sequence,
                start = view.starts[view.chosen].item, programs = ChosenPrograms(view),
                variables = ChosenVariables(view), banks = source.banks]
        {
            const edit::MappedVariables mapped = edit::WithVariables(sequence, start, variables);
            const uint32_t tracks = edit::TracksToAdd(*file, places, sound, mapped.sequence, mapped.start);
            return edit::ReplaceSequence(*file, places, sound, mapped.sequence, mapped.start, programs, "Preview",
                                         tracks, banks);
        };
        target.sound = source.sound;
    }
    else if (view.result)
    {
        // A sequence in the place of a sound's sequence.
        make = [result = *view.result]
        {
            return result;
        };
        target.sound = source.sound;
        target.mixing = format::MixingOf(*source.file, source.sound);
    }
    else
    {
        return;
    }

    StartPreview(std::move(make), target);
}

void App::StartPreview(std::function<format::File()> make, PreviewTarget target)
{
    // The work builds the version's archive with the group files that the file keeps. The zones' reader builds the
    // archive with them too. A sequence sound plays with the file's effects at the buses' volumes. Each item plays in
    // the target's mixing mode.
    const BusEffects* effects = Effects();
    const auto work = [build = std::move(make), target, name = file_name_,
                       aux = edit::ModelEffects(effects != nullptr ? *effects : BusEffects{}), volumes = BusVolumes(),
                       mixing = edit::ModelMixing(target.mixing)]
    {
        PreviewWork done;
        try
        {
            const format::File version = build();
            const std::shared_ptr<const ntdlib::sar::SoundArchive> archive =
                song::LoadArchive(format::ExportArchive(version), name, format::ExportGroupFiles(version));
            if (target.sound && archive->Sounds().at(*target.sound).type == ntdlib::sar::SoundType::kSequence)
            {
                auto sequence = std::make_shared<const song::Sequence>(song::ReadSequence(*archive, *target.sound));
                done.renderer =
                    std::make_unique<song::SequencePreview>(archive, std::move(sequence), aux, volumes, mixing);
            }
            else if (target.sound)
            {
                done.renderer =
                    std::make_unique<song::SoundPreview>(archive, *target.sound, std::vector<uint8_t>(), mixing);
            }
            else if (target.bank)
            {
                const std::vector<uint32_t> programs = song::ProgramsOf(*archive, *target.bank);
                if (programs.empty())
                {
                    done.note = "The bank holds no instrument. Play has nothing to play.";
                    return done;
                }

                done.renderer = std::make_unique<song::NotePreview>(archive, *target.bank, programs.front(), mixing);
            }
            else if (target.wave_archive)
            {
                done.renderer = std::make_unique<song::WavePreview>(*archive, *target.wave_archive, 0);
            }
        }
        catch (const std::exception& error)
        {
            done.error = error.what();
        }

        return done;
    };
    preview_work_ = std::async(std::launch::async, work);
    replace_->view.preparing = true;
}

void App::OpenWaveImport(const std::string& path, bool wav)
{
    if (!IsEnabled(wav ? Command::kImportAudioFile : Command::kImportWave))
    {
        return;
    }

    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + name + ".");
        return;
    }

    // On the Wii, the new wave goes after the waves of the last wave data that Hokuto reads.
    if (wii_archive_)
    {
        std::optional<uint32_t> last;
        for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(*wii_archive_))
        {
            try
            {
                song::WaveCountOf(*wii_archive_, wave_archive.file_id);
                last = wave_archive.file_id;
            }
            catch (const std::exception&)
            {
                // The window offers the wave data all the same, and says why it can't take the wave.
            }
        }
        if (!last)
        {
            ShowMessage("Hokuto can't import " + name + ". It can't read the wave data of any bank or wave sound.");
            return;
        }

        replace_ = WiiWaveImport(std::move(bytes), name, *last, ntdlib::sar::WaveEncoding::kDspAdpcm, wav);
        return;
    }

    // The new wave goes after the waves of the archive's last wave archive that Hokuto reads, until the user chooses
    // another in the Import window. A WAV file's wave is DSP-ADPCM until the user chooses another encoding.
    std::optional<uint32_t> last;
    for (uint32_t i = 0; i < archive_->WaveArchives().size(); i++)
    {
        try
        {
            if (!archive_->FileData(archive_->WaveArchives()[i].file_id).empty())
            {
                song::WaveCountOf(*archive_, i);
                last = i;
            }
        }
        catch (const std::exception&)
        {
            // The window offers the wave archive all the same, and says why it can't take the wave.
        }
    }
    if (!last)
    {
        ShowMessage("Hokuto can't import " + name + ". It can't read any wave archive of the archive.");
        return;
    }

    replace_ = WaveImport(std::move(bytes), name, *last, ntdlib::sar::WaveEncoding::kDspAdpcm, wav);
}

std::optional<App::Replacement> App::WaveImport(std::vector<uint8_t> bytes, const std::string& name,
                                                uint32_t wave_archive, ntdlib::sar::WaveEncoding encoding, bool wav,
                                                std::optional<uint32_t> rate)
{
    edit::IncomingWave incoming;
    try
    {
        incoming =
            wav ? edit::ReadIncomingWave(bytes, *archive_, wave_archive, std::nullopt, encoding, std::nullopt, rate)
                : edit::ReadIncomingWaveFile(bytes, *archive_, wave_archive);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't import " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The wave archive with the new wave after its waves, and what it changes. The edit starts from the version that
    // the history has reached. The Import window says why it can't be made.
    ReplaceSource source;
    source.file = history_->Current();
    source.archive = archive_;
    source.sound = wave_archive;
    source.file_name = name;
    uint32_t wave = 0;
    edit::IncomingFile waves;
    waves.kind = edit::FileKind::kWaveArchive;
    edit::FileChanges changes;
    std::optional<format::File> result;
    std::string error;
    try
    {
        wave = static_cast<uint32_t>(song::WaveCountOf(*archive_, wave_archive));
        waves.bytes = edit::WaveArchiveWith(*archive_, wave_archive, std::nullopt, incoming.bytes);
        changes = edit::ChangesOf(*source.file, *archive_, wave_archive, waves);
        result = edit::WithIncomingWave(*source.file, *archive_, wave_archive, std::nullopt, incoming);
    }
    catch (const std::exception& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, wave, std::move(bytes), std::nullopt, {}, true};
    replacement.view = MakeNewWaveView(replacement.source, wave_archive, wave, incoming, wav, changes,
                                       waves.bytes.size(), std::move(result), std::move(error));

    return replacement;
}

std::optional<App::Replacement> App::WiiWaveImport(std::vector<uint8_t> bytes, const std::string& name,
                                                   uint32_t file_id, ntdlib::sar::WaveEncoding encoding, bool wav,
                                                   std::optional<uint32_t> rate)
{
    // A WAV file's wave takes the version of the wave data's first wave. A wave file of another console goes in
    // converted.
    edit::IncomingWave incoming;
    std::vector<ntdlib::rsar::WaveFile> waves;
    try
    {
        waves = edit::WavesOf(*wii_archive_, file_id);
        const uint16_t version = waves.empty() ? ntdlib::rsar::WaveFile().version : waves.front().version;
        incoming = wav ? edit::ReadIncomingWiiWave(bytes, version, encoding, std::nullopt, rate)
                       : edit::ReadIncomingWiiWaveFile(bytes);
    }
    catch (const std::exception& error)
    {
        ShowMessage("Hokuto can't import " + name + ". " + format::AsSentence(error.what()));
        return std::nullopt;
    }

    // The wave data with the new wave after its waves. The edit starts from the version that the history has reached.
    // The Import window says why it can't be made.
    ReplaceSource source;
    source.file = history_->Current();
    source.wii = wii_archive_;
    source.sound = file_id;
    source.file_name = name;
    const auto wave = static_cast<uint32_t>(waves.size());
    std::optional<format::File> result;
    std::string error;
    try
    {
        result = edit::WithIncomingWiiWave(*source.file, *wii_archive_, file_id, std::nullopt, incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, wave, std::move(bytes), std::nullopt, {}, true};
    replacement.view =
        MakeWiiNewWaveView(replacement.source, file_id, wave, incoming, wav, std::move(result), std::move(error));

    return replacement;
}

void App::OpenStreamImport(const std::string& path)
{
    if (!IsEnabled(Command::kImportStream))
    {
        return;
    }

    const std::filesystem::path file = xsflib::PathFromUtf8(path);
    const std::string file_name = xsflib::Utf8(file.filename());
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(file, bytes))
    {
        ShowMessage("Hokuto can't read " + file_name + ".");
        return;
    }
    if (song::IsAdts(bytes))
    {
        ShowMessage("Hokuto can't import " + file_name +
                    " as a new stream. It's an AAC file. File > Import > Stream… takes a WAV file or a stream file. "
                    "An AAC file goes in through Replace… of a stream sound of the 3DS that plays an AAC stream.");
        return;
    }

    // The new sound copies the settings of the archive's first stream sound, or else of its first sound. The user can
    // choose another in the Import window. The name comes from the file's, without the encoding that the tools' names
    // hold. A WAV file's stream is DSP-ADPCM until the user chooses another encoding there.
    uint32_t model = 0;
    std::filesystem::path stem = file.stem();
    if (xsflib::AsciiLower(xsflib::Utf8(stem.extension())) == ".dspadpcm")
    {
        stem = stem.stem();
    }
    if (wii_archive_)
    {
        // A name that the archive's sounds, banks or players hold takes a number after it.
        const auto stream_sound = std::ranges::find_if(wii_archive_->Sounds(), [](const ntdlib::rsar::SoundInfo& sound)
                                                       { return sound.type == ntdlib::rsar::SoundType::kStream; });
        if (stream_sound != wii_archive_->Sounds().end())
        {
            model = static_cast<uint32_t>(stream_sound - wii_archive_->Sounds().begin());
        }
        const std::string first = xsflib::Utf8(stem).substr(0, 200);
        std::string name = first;
        for (int number = 2; WiiNameTaken(*wii_archive_, name); number++)
        {
            name = first + "_" + std::to_string(number);
        }
        replace_ = WiiStreamImport(std::move(bytes), file_name, model, name, ntdlib::sar::WaveEncoding::kDspAdpcm);
        return;
    }

    const auto stream_sound = std::ranges::find_if(archive_->Sounds(), [](const ntdlib::sar::SoundInfo& sound)
                                                   { return sound.type == ntdlib::sar::SoundType::kStream; });
    if (stream_sound != archive_->Sounds().end())
    {
        model = static_cast<uint32_t>(stream_sound - archive_->Sounds().begin());
    }
    const std::string name = FirstName(*history_->Current(), *archive_, xsflib::Utf8(stem) + ".bfstm");
    replace_ = StreamImport(std::move(bytes), file_name, model, name, ntdlib::sar::WaveEncoding::kDspAdpcm);
}

std::optional<App::Replacement> App::StreamImport(std::vector<uint8_t> bytes, const std::string& file_name,
                                                  uint32_t model, const std::string& name,
                                                  ntdlib::sar::WaveEncoding encoding,
                                                  std::optional<edit::IncomingStream> incoming)
{
    const bool wav = !edit::IsStreamFile(bytes);
    if (!incoming)
    {
        try
        {
            const ntdlib::sar::Flavor flavor = archive_->ReadOptions().flavor;
            const uint32_t version = edit::IncomingStreamVersion(*history_->Current(), flavor);
            incoming = wav ? edit::ReadIncomingStreamWav(bytes, flavor, version, encoding)
                           : edit::ReadIncomingStreamFile(bytes, flavor, version);
        }
        catch (const std::exception& error)
        {
            ShowMessage("Hokuto can't import " + file_name + ". " + format::AsSentence(error.what()));
            return std::nullopt;
        }
    }

    // The sounds to copy: the stream sounds first. The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.archive = archive_;
    source.file_name = file_name;
    for (const bool streams : {true, false})
    {
        for (std::size_t i = 0; i < archive_->Sounds().size(); i++)
        {
            if ((archive_->Sounds()[i].type == ntdlib::sar::SoundType::kStream) == streams)
            {
                source.models.push_back(static_cast<uint32_t>(i));
            }
        }
    }
    source.sound = model < archive_->Sounds().size() ? model : source.models.front();

    // The new sound's stream, its path and its tracks.
    std::optional<edit::NewStream> made;
    std::string error;
    try
    {
        made = edit::MakeNewStream(*archive_, source.sound, name, *incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.view = MakeStreamImportView(replacement.source, *incoming, wav, name, made, error);
    replacement.incoming_stream = std::move(incoming);
    replacement.new_stream = std::move(made);

    return replacement;
}

std::optional<App::Replacement> App::WiiStreamImport(std::vector<uint8_t> bytes, const std::string& file_name,
                                                     uint32_t model, const std::string& name,
                                                     ntdlib::sar::WaveEncoding encoding,
                                                     std::optional<edit::IncomingStream> incoming)
{
    // A stream file of the Wii goes in as it is, and one of a later console converted. A WAV file goes in as a stream
    // that NW4R's tools would lay out.
    const bool wav = !edit::IsWiiStreamFile(bytes) && !edit::IsStreamFile(bytes);
    if (!incoming)
    {
        try
        {
            incoming = wav ? edit::ReadIncomingWiiStreamWav(bytes, encoding) : edit::ReadIncomingWiiStreamFile(bytes);
        }
        catch (const std::exception& error)
        {
            ShowMessage("Hokuto can't import " + file_name + ". " + format::AsSentence(error.what()));
            return std::nullopt;
        }
    }

    // The sounds to copy: the stream sounds first. The edit starts from the version that the history has reached.
    ReplaceSource source;
    source.file = history_->Current();
    source.wii = wii_archive_;
    source.file_name = file_name;
    for (const bool streams : {true, false})
    {
        for (std::size_t i = 0; i < wii_archive_->Sounds().size(); i++)
        {
            if ((wii_archive_->Sounds()[i].type == ntdlib::rsar::SoundType::kStream) == streams)
            {
                source.models.push_back(static_cast<uint32_t>(i));
            }
        }
    }
    source.sound = model < wii_archive_->Sounds().size() ? model : source.models.front();
    source.refused = edit::WiiAdditionsRefused(*source.file);

    // The new sound's stream, its path and its entry.
    std::optional<edit::NewStream> made;
    std::string error;
    try
    {
        made = edit::MakeNewWiiStream(*wii_archive_, source.sound, name, *incoming);
    }
    catch (const std::invalid_argument& failure)
    {
        error = failure.what();
    }
    Replacement replacement{std::move(source), {}, std::nullopt, std::nullopt, std::move(bytes), std::nullopt, {}};
    replacement.view = MakeWiiStreamImportView(replacement.source, *incoming, wav, name, made, error);
    replacement.incoming_stream = std::move(incoming);
    replacement.new_stream = std::move(made);

    return replacement;
}

void App::MakeStreamImport()
{
    // The edit builds the archive again with the new sound. The browser then selects the new sound.
    const ReplaceSource& source = replace_->source;
    const std::string name = NameOf(replace_->view);
    if (source.wii)
    {
        const auto make = [file = source.file, model = source.sound, name, stream = *replace_->new_stream]
        {
            return edit::AddWiiStreamSound(*file, model, name, stream);
        };
        StartEdit(make, "Import Stream", name + " from " + source.file_name,
                  static_cast<uint32_t>(source.wii->Sounds().size()));
        return;
    }

    const auto make = [file = source.file, model = source.sound, name, stream = *replace_->new_stream]
    {
        return edit::AddStreamSound(*file, model, name, stream);
    };
    StartEdit(make, "Import Stream", name + " from " + source.file_name,
              static_cast<uint32_t>(source.file->native->tables->sounds.size()));
}

void App::ExportTo(const std::string& path)
{
    if (!export_ || !export_->done)
    {
        return;
    }

    // The files go where the user chose, with the target's extension. A file that can't be written leaves the window
    // open for another try.
    const ExportWork& work = *export_->done;
    try
    {
        switch (export_->Target())
        {
        case ExportTarget::kSoundArchive:
            {
                // A game's archive can end in .fsst or .fspj. A name that ends in either keeps it. Another name ends in
                // the extension of the archive's console: .brsar on the Wii, .bcsar on the 3DS and .bfsar on the Wii U
                // and the Switch.
                const std::string extension = xsflib::AsciiLower(xsflib::Utf8(xsflib::PathFromUtf8(path).extension()));
                const bool archive = extension == ".fsst" || extension == ".fspj";
                const std::string_view magic(reinterpret_cast<const char*>(work.archive.data()),
                                             std::min<std::size_t>(work.archive.size(), 4));
                const char* console = magic == "RSAR"                                                    ? ".brsar"
                                      : song::FlavorOfArchive(work.archive) == ntdlib::sar::Flavor::kCtr ? ".bcsar"
                                                                                                         : ".bfsar";
                const std::filesystem::path file = xsflib::PathFromUtf8(archive ? path : WithExtension(path, console));

                // The streams and the group files go beside the archive. A SARC archive beside it, or beside the file
                // that the user opened, takes the group file of a group whose file it holds. A group file whose game's
                // file lies in the userManagementFiles folder there goes into that folder. The program asks before it
                // writes over a file, and over the archive when the name lacked its extension.
                const std::filesystem::path folder = file.parent_path();
                std::vector<format::FileBeside> groups;
                if (!work.group_files.empty())
                {
                    std::vector<std::filesystem::path> folders = {folder};
                    if (!export_->source_folder.empty())
                    {
                        folders.push_back(export_->source_folder);
                    }
                    groups = song::PlaceGroupFiles(
                        format::GroupFileOutputs(work.group_files, work.archive, song::FlavorOfArchive(work.archive),
                                                 song::SarcArchivesIn(folders)),
                        folders);
                }
                const std::array<const std::vector<format::FileBeside>*, 2> lists = {&work.files, &groups};
                std::vector<std::string> existing;
                if (AddedNameExists(path, file))
                {
                    existing.push_back(xsflib::Utf8(file.filename()));
                }
                for (const std::vector<format::FileBeside>* list : lists)
                {
                    for (const auto& [name, bytes] : *list)
                    {
                        std::error_code error;
                        if (std::filesystem::exists(folder / xsflib::PathFromUtf8(name), error))
                        {
                            existing.push_back(name);
                        }
                    }
                }
                if (!existing.empty() && !AskToReplace(ReplaceQuestion(folder, existing)))
                {
                    return;
                }

                if (xsflib::WriteFile(file, work.archive))
                {
                    throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
                }

                for (const std::vector<format::FileBeside>* list : lists)
                {
                    for (const auto& [name, bytes] : *list)
                    {
                        if (const std::optional<std::string> error =
                                song::WriteStream(folder / xsflib::PathFromUtf8(name), bytes))
                        {
                            throw std::runtime_error(*error);
                        }
                    }
                }
                break;
            }

        // The save dialog asks before it replaces the MIDI file that the user names. The files beside it are asked
        // about here, and so is the MIDI file when the name lacked its extension.
        case ExportTarget::kMidiSoundFont:
        case ExportTarget::kMidiSfz:
            {
                const std::filesystem::path midi = xsflib::PathFromUtf8(WithExtension(path, ".mid"));
                const bool sound_font = export_->Target() == ExportTarget::kMidiSoundFont;
                std::vector<std::filesystem::path> files;
                if (xsflib::PathFromUtf8(path) != midi)
                {
                    files.push_back(midi);
                }
                if (sound_font)
                {
                    files.push_back(std::filesystem::path(midi).replace_extension(".sf2"));
                }
                else
                {
                    // A sample of another export stops the export before the question.
                    song::CheckSfzSamples(*work.conversion, midi);
                    for (const ntdlib::sequence_export::SfzFile& file : work.conversion->sfz.files)
                    {
                        files.push_back(midi.parent_path() / "sfz" / midi.stem() / xsflib::PathFromUtf8(file.name));
                    }
                }
                std::vector<std::string> existing;
                for (const std::filesystem::path& file : files)
                {
                    std::error_code error;
                    if (std::filesystem::exists(file, error))
                    {
                        existing.push_back(xsflib::Utf8(file.lexically_relative(midi.parent_path())));
                    }
                }
                if (!existing.empty() && !AskToReplace(ReplaceQuestion(midi.parent_path(), existing)))
                {
                    return;
                }

                if (sound_font)
                {
                    song::WriteMidiAndSoundFont(*work.conversion, midi);
                }
                else
                {
                    song::WriteMidiAndSfz(*work.conversion, midi);
                }
                break;
            }

        // The new file of the other console, in the binary form.
        case ExportTarget::kCtrSong:
        case ExportTarget::kWiiUSong:
        case ExportTarget::kSwitchSong:
        case ExportTarget::kWiiSong:
            {
                const std::filesystem::path file = xsflib::PathFromUtf8(WithExtension(path, ".hok"));
                if (!MayWriteTo(path, file))
                {
                    return;
                }

                if (xsflib::WriteFile(file, work.files.at(0).second))
                {
                    throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
                }
                break;
            }

        // An item's file, or a wave's WAV file, takes the extension of its format. A wave archive's WAV files go into
        // the chosen folder under their names.
        case ExportTarget::kItemFile:
        case ExportTarget::kCtrFile:
        case ExportTarget::kWiiUFile:
        case ExportTarget::kSwitchFile:
        case ExportTarget::kWiiFile:
        case ExportTarget::kWav:
            if (export_->item->kind == song::ItemKind::kWaveArchive && export_->Target() == ExportTarget::kWav)
            {
                const std::vector<std::pair<std::string, std::span<const uint8_t>>> files(work.files.begin(),
                                                                                          work.files.end());
                if (!WriteIntoFolder(xsflib::PathFromUtf8(path), files))
                {
                    return;
                }
            }
            else
            {
                const std::string extension = xsflib::Utf8(xsflib::PathFromUtf8(work.files.at(0).first).extension());
                const std::filesystem::path file = xsflib::PathFromUtf8(WithExtension(path, extension));
                if (!MayWriteTo(path, file))
                {
                    return;
                }

                if (xsflib::WriteFile(file, work.files[0].second))
                {
                    throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
                }

                // The files that go beside it take its name, with their extensions: the waves of a bank or a wave sound
                // file.
                std::vector<std::pair<std::string, std::span<const uint8_t>>> beside;
                for (std::size_t i = 1; i < work.files.size(); i++)
                {
                    std::filesystem::path name = file.filename();
                    name.replace_extension(xsflib::PathFromUtf8(work.files[i].first).extension());
                    beside.emplace_back(xsflib::Utf8(name), work.files[i].second);
                }
                if (!beside.empty() && !WriteIntoFolder(file.parent_path(), beside))
                {
                    return;
                }
            }
            break;

        // A bank's SoundFont goes where the user chose. Instruments past the format's limits go into several SoundFonts
        // beside it instead, each under the chosen name and its programs (song::SoundFontNames).
        case ExportTarget::kSoundFont:
            {
                const std::filesystem::path file = xsflib::PathFromUtf8(WithExtension(path, ".sf2"));
                const std::vector<std::string> names = song::SoundFontNames(*work.bank, xsflib::Utf8(file.stem()));
                if (names.size() == 1)
                {
                    if (!MayWriteTo(path, file))
                    {
                        return;
                    }

                    if (xsflib::WriteFile(file, work.bank->sound_fonts[0].sf2))
                    {
                        throw std::runtime_error(song::WriteFailure(xsflib::Utf8(file.filename())));
                    }
                    break;
                }

                std::vector<std::pair<std::string, std::span<const uint8_t>>> files;
                for (std::size_t i = 0; i < names.size(); i++)
                {
                    files.emplace_back(names[i], work.bank->sound_fonts[i].sf2);
                }
                if (!WriteIntoFolder(file.parent_path(), files))
                {
                    return;
                }
                break;
            }

        // A bank's SFZ files go into a folder with the bank's name in the chosen folder, and their samples beside it.
        case ExportTarget::kSfz:
            {
                const std::filesystem::path folder = xsflib::PathFromUtf8(path);
                const auto replace = [this, &folder](const std::vector<std::string>& existing)
                {
                    return AskToReplace(ReplaceQuestion(folder / xsflib::PathFromUtf8(export_->file_name), existing));
                };
                if (!song::WriteBankSfz(*work.bank, folder, export_->file_name, replace))
                {
                    return;
                }
                break;
            }
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return;
    }

    // A measure that still runs goes on its own and stops early. Its future would wait for it.
    DropExport();
}

bool App::WriteIntoFolder(const std::filesystem::path& folder,
                          const std::vector<std::pair<std::string, std::span<const uint8_t>>>& files)
{
    // The files that the folder holds already, by their names.
    std::vector<std::string> existing;
    for (const auto& [name, bytes] : files)
    {
        std::error_code error;
        if (std::filesystem::exists(folder / xsflib::PathFromUtf8(name), error))
        {
            existing.push_back(name);
        }
    }
    if (!existing.empty() && !AskToReplace(ReplaceQuestion(folder, existing)))
    {
        return false;
    }

    for (const auto& [name, bytes] : files)
    {
        const std::filesystem::path file = folder / xsflib::PathFromUtf8(name);
        if (xsflib::WriteFile(file, bytes))
        {
            throw std::runtime_error(song::WriteFailure(name));
        }
    }

    return true;
}

bool App::AskToReplace(const std::string& message)
{
    // Cancel answers Escape, and Replace answers Return. In SDL3's right-to-left order, Replace comes on the right on
    // macOS and on the left on Windows. Each system places a dialog's main button there.
    const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Replace"}};
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT;
    box.window = window_;
    box.title = "Hokuto";
    box.message = message.c_str();
    box.numbuttons = 2;
    box.buttons = buttons;
    int chosen = 0;
    return SDL_ShowMessageBox(&box, &chosen) && chosen == 1;
}

bool App::MayWriteTo(const std::string& chosen, const std::filesystem::path& file)
{
    return !AddedNameExists(chosen, file) ||
           AskToReplace(ReplaceQuestion(file.parent_path(), {xsflib::Utf8(file.filename())}));
}

void App::StartItemExport(const BrowserPlace& place)
{
    if (export_ || !history_ || preview_ || (!archive_ && !wii_archive_) || !archive_version_ || place.group < 0 ||
        place.item < 0)
    {
        return;
    }

    // The item at the place: a sound, a bank, a wave archive or a wave sound file, an instrument of a bank, a wave of a
    // wave archive, or a wave sound of a wave sound file. An instrument goes out with its bank as the item.
    const auto group = static_cast<std::size_t>(place.group);
    const auto index = static_cast<std::size_t>(place.item);
    if (group >= browser_items_.size() || index >= browser_items_[group].size() ||
        (place.child >= 0 && (index >= browser_children_[group].size() ||
                              static_cast<std::size_t>(place.child) >= browser_children_[group][index].size())))
    {
        return;
    }

    song::Item item;
    item.index = browser_items_[group][index];
    switch (group)
    {
    case kSoundsGroup:
        item.kind = wii_archive_ ? (item.index < wii_archive_->Sounds().size() &&
                                            wii_archive_->Sounds()[item.index].type == ntdlib::rsar::SoundType::kStream
                                        ? song::ItemKind::kStream
                                        : song::ItemKind::kWaveSound)
                    : item.index < archive_->Sounds().size() &&
                            archive_->Sounds()[item.index].type == ntdlib::sar::SoundType::kStream
                        ? song::ItemKind::kStream
                        : song::ItemKind::kWaveSound;
        break;

    case kSequencesGroup:
        item.kind = song::ItemKind::kSequence;
        break;

    case kBanksGroup:
        item.kind = song::ItemKind::kBank;
        break;

    case kWaveSoundFilesGroup:
        item.kind = place.child >= 0 ? song::ItemKind::kWaveSound : song::ItemKind::kWaveSoundFile;
        item.index =
            place.child >= 0 ? browser_children_[group][index].at(static_cast<std::size_t>(place.child)) : item.index;
        break;

    default:
        item.kind = place.child >= 0 ? song::ItemKind::kWave : song::ItemKind::kWaveArchive;
        item.wave = place.child >= 0 ? browser_children_[group][index].at(static_cast<std::size_t>(place.child)) : 0;
        break;
    }

    Export job;
    job.view = wii_archive_ ? ItemExportView(*wii_archive_, item) : ItemExportView(*archive_, item);
    if (group == kBanksGroup && place.child >= 0)
    {
        job.program = browser_children_[group][index].at(static_cast<std::size_t>(place.child));
        job.last_program = browser_children_[group][index].back();
        job.view = InstrumentExportView(job.view.source, *job.program);
    }
    job.item = item;
    StartItemWork(job);
    export_ = std::move(job);
}

void App::StartWiiItemWork(Export& job)
{
    // As for the other consoles, the dialog offers the file's name. A wave's name pads its index to the digits of its
    // wave archive's last wave. A bank's SoundFont takes the bank's name, and so does the folder of its SFZ files. An
    // instrument's files add its program, with the digits that the bank's SFZ files give it (ProgramPart).
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    std::size_t waves = 0;
    if (item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive)
    {
        try
        {
            waves = song::WaveCountOf(*wii_archive_, item.index);
        }
        catch (const std::exception&)
        {
            // The work reports the wave archive that can't be read.
        }
    }

    // A stream takes the name of its file beside the archive. Its file for another console takes the extension of that
    // console's format.
    job.file_name = item.kind == song::ItemKind::kWave && target == ExportTarget::kWav
                        ? song::WavFileName(*wii_archive_, item.index, item.wave, waves)
                    : item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav
                        ? song::WaveSoundWavFileName(*wii_archive_, item.index)
                        : song::ItemFileName(*wii_archive_, item, waves);
    if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
    {
        job.file_name = WithoutExtension(job.file_name);
        job.file_name += job.program ? ProgramPart(*job.program, job.last_program) : "";
        job.file_name += target == ExportTarget::kSoundFont ? ".sf2" : "";
    }
    const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
    if (console || (item.kind == song::ItemKind::kStream && target == ExportTarget::kWav))
    {
        job.file_name = WithoutExtension(job.file_name);
        job.file_name += console ? "." + xsflib::AsciiLower(song::ItemFormat(item.kind, *console)) : ".wav";
    }

    // The work exports the version that the history has reached. The archive that the zones play serves when it came
    // from the version, unless the version has timelines: their files go out with their subroutines. Otherwise the work
    // builds the version's archive first. A bank's instruments play by the rules of the file's build of NW4R SND. A
    // wave archive's WAV export leaves out the waves that can't be decoded, and warns of each. The file keeps a stream,
    // and the archive names it.
    const std::shared_ptr<const format::File> version = history_->Current();
    const bool timelines = version->timelines && !version->timelines->empty();
    const auto make = [version,
                       archive = SameArchive(*version, *archive_version_) && !timelines ? wii_archive_ : nullptr,
                       name = file_name_, item, target, file_name = job.file_name, program = job.program]() mutable
    {
        ExportWork work;
        try
        {
            if (!archive)
            {
                archive = song::LoadWiiArchive(format::ExportArchive(edit::FoldTimelines(*version)), name,
                                               ntdlib::rvl::snd::kRsarOptions);
            }
            std::vector<std::string> found;
            if (item.kind == song::ItemKind::kStream)
            {
                const std::vector<uint8_t>& bytes = StreamBytes(*version, *archive, item.index);
                const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
                work.files.emplace_back(
                    file_name,
                    target == ExportTarget::kItemFile ? bytes
                    : console ? song::ConvertStreamFile(bytes, *console, song::ExportVersions(item.kind, *console).file)
                              : song::StreamAsWav(bytes));
                found = console ? StreamConversionWarnings(bytes, *console)
                        : target == ExportTarget::kWav
                            ? WiiStreamWavWarnings(bytes, format::BuildOfWiiArchive(*version->native))
                            : std::vector<std::string>();
            }
            else if (target == ExportTarget::kItemFile)
            {
                // A bank file or a wave sound file takes its waves along, in an RWAR beside it under its name.
                work.files.emplace_back(file_name, song::ItemBytes(*archive, item));
                const bool bank = item.kind == song::ItemKind::kBank;
                if (bank || item.kind == song::ItemKind::kWaveSound || item.kind == song::ItemKind::kWaveSoundFile)
                {
                    const uint32_t file_id =
                        bank ? archive->Banks().at(item.index).file_id : archive->Sounds().at(item.index).file_id;
                    std::filesystem::path beside = xsflib::PathFromUtf8(file_name);
                    beside.replace_extension(".brwar");
                    try
                    {
                        work.files.emplace_back(xsflib::Utf8(beside),
                                                song::ItemBytes(*archive, {song::ItemKind::kWaveArchive, file_id, 0}));
                    }
                    catch (const std::exception& error)
                    {
                        found.push_back(format::AsSentence(error.what()) + " Its waves stay behind.");
                    }
                }
            }
            else if (const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target))
            {
                // A bank takes its waves along, in a wave archive file beside it under its name.
                const ntdlib::SndBuild build = format::BuildOfWiiArchive(*version->native);
                edit::ConvertedItem converted;
                if (item.kind == song::ItemKind::kSequence)
                {
                    converted = edit::LaterSequenceFileOf(*archive, item.index, build, *console);
                }
                else if (item.kind == song::ItemKind::kBank)
                {
                    try
                    {
                        converted = edit::LaterBankFilesOf(*archive, item.index, build, *console);
                    }
                    catch (const std::exception& error)
                    {
                        throw std::runtime_error("Hokuto can't convert the bank for the " +
                                                 song::ConsoleName(*console) + ". " + format::AsSentence(error.what()));
                    }
                }
                else
                {
                    converted.file = song::LaterWaveFileOf(*archive, item, *console);
                }
                work.files.emplace_back(file_name, std::move(converted.file));
                if (!converted.waves.empty())
                {
                    std::filesystem::path beside = xsflib::PathFromUtf8(file_name);
                    beside.replace_extension(
                        "." + xsflib::AsciiLower(song::ItemFormat(song::ItemKind::kWaveArchive, *console)));
                    work.files.emplace_back(xsflib::Utf8(beside), std::move(converted.waves));
                }
                found = std::move(converted.warnings);
            }
            else if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
            {
                // An instrument goes out from a copy of its bank that holds it alone, at its program.
                if (program)
                {
                    const format::File folded = edit::FoldTimelines(*version);
                    const format::File alone =
                        edit::WithBankModel(folded, *archive, item.index,
                                            InstrumentAlone(edit::BankModelOf(*archive, item.index), *program));
                    archive = song::LoadWiiArchive(format::ExportArchive(alone), name, ntdlib::rvl::snd::kRsarOptions);
                }
                work.bank =
                    song::ConvertBank(*archive, item.index, "Hokuto", format::BuildOfWiiArchive(*version->native));
                NameInstrumentSfz(*work.bank, program, file_name);
                if (target == ExportTarget::kSoundFont)
                {
                    found = SoundFontWarnings(*work.bank, xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem()));
                }
                const std::vector<std::string> converter =
                    ConverterWarnings(work.bank->warnings, target == ExportTarget::kSfz);
                found.insert(found.end(), converter.begin(), converter.end());
            }
            else if (item.kind == song::ItemKind::kWave)
            {
                work.files.emplace_back(file_name, song::WaveAsWav(*archive, item.index, item.wave));
            }
            else if (item.kind == song::ItemKind::kWaveSound)
            {
                work.files.emplace_back(file_name, song::WaveSoundAsWav(*archive, item.index));
            }
            else
            {
                const std::size_t count = song::WaveCountOf(*archive, item.index);
                for (uint32_t wave = 0; wave < count; wave++)
                {
                    try
                    {
                        work.files.emplace_back(song::WavFileName(*archive, item.index, wave, count),
                                                song::WaveAsWav(*archive, item.index, wave));
                    }
                    catch (const std::exception& error)
                    {
                        found.push_back(format::AsSentence(error.what()) + " The folder leaves it out.");
                    }
                }
                if (work.files.empty())
                {
                    throw std::runtime_error(count == 0 ? song::ItemName(*archive, item) + " holds no waves."
                                                        : "Hokuto can't decode any wave of " +
                                                              song::ItemName(*archive, item) + ".");
                }
            }
            work.warnings = ItemWarnings(*archive, item, target, found);
        }
        catch (const std::exception& error)
        {
            work.error = error.what();
        }

        return work;
    };
    job.work = std::async(std::launch::async, make);
}

void App::StartItemWork(Export& job)
{
    if (job.Target() == ExportTarget::kMidiSoundFont || job.Target() == ExportTarget::kMidiSfz)
    {
        StartMidiWork(job, job.item->index);
        return;
    }
    if (wii_archive_)
    {
        StartWiiItemWork(job);
        return;
    }

    // The dialog offers the file's name. A wave's name pads its index to the digits of the archive's last wave. A wave
    // sound's WAV file takes the sound's name. A bank's SoundFont takes the bank's name, and so does the folder of its
    // SFZ files. An instrument's files add its program, with the digits that the bank's SFZ files give it
    // (ProgramPart). Edits leave the items' names as they are. The archive that the zones play then names them.
    const song::Item item = *job.item;
    const ExportTarget target = job.Target();
    std::size_t waves = 0;
    if (item.kind == song::ItemKind::kWave || item.kind == song::ItemKind::kWaveArchive)
    {
        try
        {
            waves = song::WaveCountOf(*archive_, item.index);
        }
        catch (const std::exception&)
        {
            // The work reports the wave archive that can't be read.
        }
    }
    if (item.kind == song::ItemKind::kWave && target == ExportTarget::kWav)
    {
        job.file_name = song::WavFileName(*archive_, item.index, item.wave, waves);
    }
    else if (item.kind == song::ItemKind::kWaveSound && target == ExportTarget::kWav)
    {
        job.file_name = song::WaveSoundWavFileName(*archive_, item.index);
    }
    else if (item.kind == song::ItemKind::kStream && target == ExportTarget::kWav)
    {
        job.file_name = xsflib::Utf8(xsflib::PathFromUtf8(song::ItemFileName(*archive_, item)).stem()) + ".wav";
    }
    else
    {
        job.file_name = song::ItemFileName(*archive_, item, waves);
    }
    if (const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target))
    {
        // A file for another console takes the extension of that console's format.
        job.file_name = WithoutExtension(job.file_name);
        job.file_name += "." + xsflib::AsciiLower(song::ItemFormat(item.kind, *console));
    }
    else if (target == ExportTarget::kWiiFile)
    {
        job.file_name = WithoutExtension(job.file_name) + "." + xsflib::AsciiLower(song::WiiItemFormat(item.kind));
    }
    if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
    {
        job.file_name = WithoutExtension(job.file_name);
        job.file_name += job.program ? ProgramPart(*job.program, job.last_program) : "";
        job.file_name += target == ExportTarget::kSoundFont ? ".sf2" : "";
    }

    // The work exports the version that the history has reached. The archive that the zones play serves when it came
    // from the version, unless the version has timelines: their files go out with their subroutines. Otherwise the work
    // builds the version's archive first. A wave archive's WAV export leaves out the waves that can't be decoded, and
    // warns of each. A bank's export lists what the converter reports.
    const std::shared_ptr<const format::File> version = history_->Current();
    const bool timelines = version->timelines && !version->timelines->empty();
    const auto make = [version, archive = SameArchive(*version, *archive_version_) && !timelines ? archive_ : nullptr,
                       name = file_name_, item, target, file_name = job.file_name, program = job.program]() mutable
    {
        ExportWork work;
        try
        {
            if (!archive)
            {
                const format::File folded = edit::FoldTimelines(*version);
                archive = song::LoadArchive(format::ExportArchive(folded), name, format::ExportGroupFiles(folded));
            }
            std::vector<std::string> found;
            if (item.kind == song::ItemKind::kStream)
            {
                // The file keeps the stream. The archive names it. An AAC stream takes the loop that the sound's entry
                // gives.
                const std::vector<uint8_t>& bytes = StreamBytes(*version, *archive, item.index);
                const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target);
                const song::AacStream aac = song::IsAdts(bytes)
                                                ? song::AacStreamOf(*archive, item.index).value_or(song::AacStream())
                                                : song::AacStream();
                work.files.emplace_back(
                    file_name, target == ExportTarget::kItemFile  ? bytes
                               : target == ExportTarget::kWiiFile ? song::WiiStreamFile(bytes, aac)
                               : console ? song::ConvertStreamFile(bytes, *console,
                                                                   song::ExportVersions(item.kind, *console).file, aac)
                                         : song::StreamAsWav(bytes, aac));
                if (console)
                {
                    found = StreamConversionWarnings(bytes, *console);
                }
            }
            else if (target == ExportTarget::kItemFile)
            {
                const std::span<const uint8_t> bytes = song::ItemFile(*archive, item);
                work.files.emplace_back(file_name, std::vector<uint8_t>(bytes.begin(), bytes.end()));
            }
            else if (target == ExportTarget::kWiiFile)
            {
                // A bank file or a wave sound file of the Wii takes its waves along, in an RWAR beside it under its
                // name.
                edit::ConvertedItem converted;
                switch (item.kind)
                {
                case song::ItemKind::kSequence:
                    converted = edit::WiiSequenceFileOf(*archive, item.index);
                    break;
                case song::ItemKind::kBank:
                    converted = edit::WiiBankFilesOf(*archive, item.index);
                    break;
                case song::ItemKind::kWaveSound:
                case song::ItemKind::kWaveSoundFile:
                    converted = edit::WiiWaveSoundFilesOf(*archive, item.index);
                    break;
                case song::ItemKind::kWaveArchive:
                case song::ItemKind::kWave:
                case song::ItemKind::kStream:
                    converted.file = song::WiiWaveFileOf(*archive, item);
                    break;
                }

                work.files.emplace_back(file_name, std::move(converted.file));
                if (!converted.waves.empty())
                {
                    std::filesystem::path beside = xsflib::PathFromUtf8(file_name);
                    beside.replace_extension(".brwar");
                    work.files.emplace_back(xsflib::Utf8(beside), std::move(converted.waves));
                }
                found = std::move(converted.warnings);
            }
            else if (const std::optional<ntdlib::sar::Flavor> console = ConsoleOf(target))
            {
                // A sequence's biquad filters take the types and values that come nearest on the other console.
                if (item.kind == song::ItemKind::kSequence)
                {
                    song::ConvertedSequence converted = song::OtherConsoleSequenceFile(*archive, item.index, *console);
                    work.files.emplace_back(file_name, std::move(converted.file));
                    const song::Console from{.flavor = archive->ReadOptions().flavor};
                    if (const std::optional<std::string> warning =
                            song::BiquadWarning(converted.biquads, from, {.flavor = *console}))
                    {
                        found.push_back(*warning);
                    }
                }
                else
                {
                    work.files.emplace_back(file_name, song::OtherConsoleFile(*archive, item, *console));
                }
            }
            else if (target == ExportTarget::kSoundFont || target == ExportTarget::kSfz)
            {
                // An instrument goes out from a copy of its bank that holds it alone, at its program.
                if (program)
                {
                    const format::File folded = edit::FoldTimelines(*version);
                    const format::File alone =
                        edit::WithBankModel(folded, *archive, item.index,
                                            InstrumentAlone(edit::BankModelOf(folded, *archive, item.index), *program));
                    archive = song::LoadArchive(format::ExportArchive(alone), name, format::ExportGroupFiles(alone));
                }
                work.bank = song::ConvertBank(*archive, item.index, "Hokuto");
                NameInstrumentSfz(*work.bank, program, file_name);
                if (target == ExportTarget::kSoundFont)
                {
                    found = SoundFontWarnings(*work.bank, xsflib::Utf8(xsflib::PathFromUtf8(file_name).stem()));
                }
                const std::vector<std::string> converter =
                    ConverterWarnings(work.bank->warnings, target == ExportTarget::kSfz);
                found.insert(found.end(), converter.begin(), converter.end());
            }
            else if (item.kind == song::ItemKind::kWave)
            {
                work.files.emplace_back(file_name, song::WaveAsWav(*archive, item.index, item.wave));
            }
            else if (item.kind == song::ItemKind::kWaveSound)
            {
                work.files.emplace_back(file_name, song::WaveSoundAsWav(*archive, item.index));
            }
            else
            {
                const std::size_t count = song::WaveCountOf(*archive, item.index);
                for (uint32_t wave = 0; wave < count; wave++)
                {
                    try
                    {
                        work.files.emplace_back(song::WavFileName(*archive, item.index, wave, count),
                                                song::WaveAsWav(*archive, item.index, wave));
                    }
                    catch (const std::exception& error)
                    {
                        found.push_back(format::AsSentence(error.what()) + " The folder leaves it out.");
                    }
                }

                // A wave archive without its file or without waves gives no WAV files. ItemFile throws when the archive
                // lacks the file.
                if (work.files.empty())
                {
                    song::ItemFile(*archive, item);
                    throw std::runtime_error(count == 0 ? song::ItemName(*archive, item) + " holds no waves."
                                                        : "Hokuto can't decode any wave of " +
                                                              song::ItemName(*archive, item) + ".");
                }
            }
            work.warnings = ItemWarnings(*archive, item, target, found);
        }
        catch (const std::exception& error)
        {
            work.error = error.what();
        }

        return work;
    };
    job.work = std::async(std::launch::async, make);
}

void App::UpdateTitle()
{
    const bool modified = history_ && !preview_ && history_->Modified();
    const std::string title = "Hokuto - " + file_name_ + (modified ? "*" : "");
    SDL_SetWindowTitle(window_, title.c_str());
}

const format::File* App::Document() const
{
    return history_ ? history_->Current().get() : nullptr;
}

const BusEffects* App::Effects() const
{
    const format::File* document = Document();
    return document != nullptr ? &document->aux_effects : nullptr;
}

void App::DrawEffects()
{
    const BusEffects* effects = Effects();
    if (effects == nullptr)
    {
        fx_.clicked.reset();
        effect_window_.reset();
        effect_draft_.reset();
        return;
    }

    // A choice in an empty insert's menu adds the effect, and a click on an effect opens it. Either way, the effect's
    // window opens in place of any other. The menu offers the kinds of the engine that plays the shown sound.
    if (const std::optional<EffectPlace> clicked = std::exchange(fx_.clicked, std::nullopt))
    {
        const EffectPlace place{clicked->channel, std::min(clicked->index, (*effects)[clicked->channel].size())};
        if (place.index == (*effects)[place.channel].size())
        {
            BusEffects added = *effects;
            added[place.channel].push_back(NewEffect(*Document(), PlayedEngine(), fx_.kind));
            ChangeEffects({"Add Effect", EffectTitle(place, added[place.channel].back())}, added);
            effects = Effects();
        }
        effect_window_ = place;
        effect_draft_.reset();
        effect_changing_ = false;
    }

    // The window closes once its effect has gone, and once its channel has gone from the mixer, such as bus C on the
    // 3DS's engine.
    if (!effect_window_ || effect_window_->channel >= fx_.count ||
        effect_window_->index >= (*effects)[effect_window_->channel].size())
    {
        effect_window_.reset();
        effect_draft_.reset();
        return;
    }

    // The draft follows the file, but not while a drag or a typed value is under way. A move in the history to other
    // effects shows in the window even during a drag (FollowHistory).
    const EffectPlace place = *effect_window_;
    const format::Effect& current = (*effects)[place.channel][place.index];
    if (!effect_changing_ || !effect_draft_)
    {
        effect_draft_ = current;
    }
    bool open = true;
    const EffectEdit edit = DrawEffectWindow(place, *effect_draft_, &open);
    effect_changing_ = edit == EffectEdit::kChanging;
    if ((edit == EffectEdit::kChanged || edit == EffectEdit::kPreset) && *effect_draft_ != current)
    {
        BusEffects changed = *effects;
        changed[place.channel][place.index] = *effect_draft_;
        ChangeEffects(EffectChangeNames(place, edit, current, *effect_draft_), changed);
    }
    else if (edit == EffectEdit::kRemoved)
    {
        BusEffects removed = *effects;
        std::vector<format::Effect>& channel = removed[place.channel];
        channel.erase(channel.begin() + static_cast<std::ptrdiff_t>(place.index));
        ChangeEffects({"Remove Effect", EffectTitle(place, current)}, removed);
    }
    if (!open || edit == EffectEdit::kRemoved)
    {
        effect_window_.reset();
        effect_draft_.reset();
    }
}

void App::OpenEditor(const BrowserPlace& place)
{
    const auto group = static_cast<std::size_t>(place.group);
    if ((!archive_ && !wii_archive_) || place.group < 0 || place.item < 0 || group >= browser_items_.size() ||
        static_cast<std::size_t>(place.item) >= browser_items_[group].size())
    {
        return;
    }

    // A bank opens in the Bank Editor, and an instrument opens there selected. A wave opens in its properties, and so
    // does a stream sound's stream. On the Wii, a wave archive is the wave data of a bank or of a wave sound file, by
    // its file's ID.
    const auto item = static_cast<std::size_t>(place.item);
    const uint32_t index = browser_items_[group][item];
    const bool listed =
        place.child >= 0 && static_cast<std::size_t>(place.child) < browser_children_[group][item].size();
    const std::optional<uint32_t> child =
        listed ? std::optional<uint32_t>(browser_children_[group][item][static_cast<std::size_t>(place.child)])
               : std::nullopt;
    if (group == kBanksGroup)
    {
        OpenBankEditor(index, child);
    }
    else if (group == kWavesGroup && child)
    {
        OpenWaveProperties(index, *child);
    }
    else if (group == kSoundsGroup && place.child < 0 && archive_ && song::StreamFileOf(*archive_, index))
    {
        OpenStreamProperties(index);
    }
    else if (group == kSoundsGroup && place.child < 0 && wii_archive_ && song::StreamFileOf(*wii_archive_, index))
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
    bank_view_version_.reset();
    bank_state_ = {};
    waiting_bank_drop_.reset();
    DropGroupSizes(bank_group_sizes_);
}

void App::UpdateBankEditor()
{
    // A bank that Project > Add Bank… added opens once the program has built the archive that holds it.
    const std::size_t banks = wii_archive_ ? wii_archive_->Banks().size() : archive_ ? archive_->Banks().size() : 0;
    if (bank_to_open_ && !EditorsWait())
    {
        const uint32_t bank = *std::exchange(bank_to_open_, std::nullopt);
        if (bank < banks)
        {
            OpenBankEditor(bank);
        }
    }
    if (!bank_editor_)
    {
        return;
    }

    const format::File* document = Document();
    if (document == nullptr || !archive_version_ || *bank_editor_ >= banks)
    {
        CloseBankEditor();
        return;
    }

    // The view follows each archive that the program builds, and each move in the history back to the archive that the
    // program holds (FollowHistory).
    if (bank_view_version_ != archive_version_)
    {
        try
        {
            bank_view_ = wii_archive_ ? MakeBankView(*archive_version_, *wii_archive_, *bank_editor_)
                                      : MakeBankView(*archive_version_, *archive_, *bank_editor_);
            bank_view_version_ = archive_version_;
            MeasureGroups(bank_group_sizes_, wii_archive_ ? wii_archive_->Banks()[*bank_editor_].file_id
                                                          : archive_->Banks()[*bank_editor_].file_id);
        }
        catch (const std::exception& error)
        {
            // The edits' refusals already say that Hokuto can't edit the bank.
            constexpr std::string_view kRefused = "Hokuto can't edit this bank. ";
            const std::string message = error.what();
            ShowMessage(message.starts_with(kRefused) ? message : std::string(kRefused) + format::AsSentence(message));
            CloseBankEditor();
            return;
        }
    }

    // Files that waited for a field's text go on the bank once the reader has stopped building and reading another
    // version (`reading_version_`). DropFilesOnBank refuses them with a message when the build failed or a window
    // waits.
    if (waiting_bank_drop_ && reading_version_ == nullptr)
    {
        const WaitingBankDrop waiting = *std::exchange(waiting_bank_drop_, std::nullopt);
        DropFilesOnBank(waiting.paths, waiting.drop);
    }

    // While the program builds the archive of another version, or another window waits, the editor takes no edits.
    const bool busy = EditorsWait();
    TakeGroupSizes(bank_group_sizes_, bank_view_->sizes);
    bool open = true;
    const BankEdit bank_edit = DrawBankEditor(*bank_view_, bank_state_, busy, &open);

    // The keyboard sounds its key on the selected instrument, over the playback. Another key, or none, lets the last
    // key go.
    std::optional<std::pair<uint32_t, int>> note;
    if (open && bank_state_.sounding >= 0 && bank_state_.program)
    {
        note = std::pair(*bank_state_.program, bank_state_.sounding);
    }
    if (note != bank_note_)
    {
        if (bank_release_)
        {
            bank_release_->store(true, std::memory_order_release);
            bank_release_.reset();
        }
        bank_note_ = note;
        if (note)
        {
            // On the Wii, the note plays by the rules of the file's build of NW4R SND.
            auto release = std::make_shared<std::atomic<bool>>(false);
            try
            {
                if (wii_archive_)
                {
                    std::optional<ntdlib::SndBuild> build;
                    if (document != nullptr && document->native)
                    {
                        build = format::BuildOfWiiArchive(*document->native);
                    }
                    PlayAudition(std::make_unique<song::NotePreview>(wii_archive_, std::vector<uint32_t>{*bank_editor_},
                                                                     0, note->first, note->second, bank_state_.velocity,
                                                                     release, build));
                }
                else
                {
                    const std::vector<uint32_t> slots = {ntdlib::sar::ItemId(ntdlib::sar::kBankItem, *bank_editor_)};
                    const format::Mixing mixing = document != nullptr ? document->mixing : format::Mixing::kStrict;
                    PlayAudition(std::make_unique<song::NotePreview>(archive_, slots, 0, note->first, note->second,
                                                                     bank_state_.velocity, release,
                                                                     edit::ModelMixing(mixing)));
                }
                bank_release_ = std::move(release);
            }
            catch (const std::exception& error)
            {
                ShowMessage(std::string("Hokuto can't play this instrument. ") + format::AsSentence(error.what()));
            }
        }
    }
    if (!open)
    {
        CloseBankEditor();
        return;
    }

    ApplyBankEdit(bank_edit);
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
        format::File file = wii_archive_
                                ? edit::WithBankModel(*Document(), *wii_archive_, *bank_editor_, *bank_edit.model)
                                : edit::WithBankModel(*Document(), *archive_, *bank_editor_, *bank_edit.model);
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
        bank_view_->entries.push_back(wii_archive_ ? edit::WaveEntryName(*wii_archive_, bank_view_->model, entry)
                                                   : edit::WaveEntryName(*archive_, bank_view_->model, entry));
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
    if (!bank_editor_ || !bank_view_ || document == nullptr || EditorsWait())
    {
        ShowMessage("Hokuto can't place the files on the bank while it builds an archive or a window waits.");
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

    if (wii_archive_)
    {
        DropFilesOnWiiBank(paths, drop);
        return;
    }

    // Each file is a WAV file or a wave file. Its wave goes after the waves of the bank's wave archive, or into a new
    // one.
    const auto wave_archive = edit::WaveArchiveOfBank(bank_view_->model, *archive_)
                                  .value_or(static_cast<uint32_t>(archive_->WaveArchives().size()));
    std::vector<edit::IncomingWave> incoming;
    for (const std::string& path : paths)
    {
        const std::filesystem::path file = xsflib::PathFromUtf8(path);
        const std::string name = xsflib::Utf8(file.filename());
        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(file, bytes))
        {
            ShowMessage("Hokuto can't read " + name + ".");
            return;
        }

        try
        {
            incoming.push_back(edit::IsWaveFile(bytes)
                                   ? edit::ReadIncomingWaveFile(bytes, *archive_, wave_archive)
                                   : edit::ReadIncomingWave(bytes, *archive_, wave_archive, std::nullopt));
        }
        catch (const std::exception& error)
        {
            ShowMessage("Hokuto can't place " + name + " on the bank. " + format::AsSentence(error.what()));
            return;
        }
    }

    // The waves make regions or instruments, and the bank names them.
    try
    {
        const edit::BankWaves added =
            edit::WithBankWaves(*document, *archive_, *bank_editor_, bank_view_->model, incoming);
        DroppedWaves dropped = WithDroppedWaves(bank_view_->model, drop, added.waves);
        format::File file = edit::WithBankModel(added.file, *archive_, *bank_editor_, dropped.bank);
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
        if (!xsflib::ReadFile(file, bytes))
        {
            ShowMessage("Hokuto can't read " + name + ".");
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
            ShowMessage("Hokuto can't place " + name + " on the bank. " + format::AsSentence(error.what()));
            return;
        }
    }

    // The waves make regions or instruments, and the bank names them.
    try
    {
        const uint32_t file_id = wii_archive_->Banks()[*bank_editor_].file_id;
        const std::vector<ntdlib::sar::WaveId> ids = edit::NewWaveIds(*wii_archive_, *bank_editor_, waves.size());
        DroppedWaves dropped = WithDroppedWaves(bank_view_->model, drop, ids);
        format::File file = edit::WithBankModel(document, *wii_archive_, *bank_editor_, dropped.bank, std::move(waves));
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
    const auto item = static_cast<std::size_t>(place.item);
    const auto& wave_archives = browser_items_[kWavesGroup];
    if (!bank_editor_ || !bank_view_ || place.group != static_cast<int>(kWavesGroup) || place.item < 0 ||
        item >= wave_archives.size() || place.child < 0 ||
        static_cast<std::size_t>(place.child) >= browser_children_[kWavesGroup][item].size())
    {
        return;
    }

    // The wave makes a region or an instrument, and the bank names it. On the Wii, the wave archive is a file's wave
    // data, by the file's ID, and a wave of another file comes into the bank as a copy.
    const ntdlib::sar::WaveId wave{ntdlib::sar::ItemId(ntdlib::sar::kWaveArchiveItem, wave_archives[item]),
                                   browser_children_[kWavesGroup][item][static_cast<std::size_t>(place.child)]};
    try
    {
        DroppedWaves dropped = WithDroppedWaves(bank_view_->model, drop, {wave});
        format::File file = wii_archive_ ? edit::WithBankModel(*Document(), *wii_archive_, *bank_editor_, dropped.bank)
                                         : edit::WithBankModel(*Document(), *archive_, *bank_editor_, dropped.bank);
        DoBankDrop(std::move(dropped), std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::OpenWaveProperties(uint32_t wave_archive, uint32_t wave)
{
    const WaveTarget target{wave_archive, wave};
    if (properties_wave_ != target)
    {
        CloseProperties();
        properties_wave_ = target;
    }
    ImGui::SetWindowFocus("###WaveProperties");
}

void App::OpenStreamProperties(uint32_t sound)
{
    if (properties_stream_ != sound)
    {
        CloseProperties();
        properties_stream_ = sound;
    }
    ImGui::SetWindowFocus("###WaveProperties");
}

void App::CloseProperties()
{
    if (wave_state_.playing)
    {
        worker_.Stop();
    }
    properties_wave_.reset();
    properties_stream_.reset();
    wave_view_.reset();
    wave_view_version_.reset();
    wave_state_ = {};
    DropGroupSizes(wave_group_sizes_);
}

void App::UpdateProperties()
{
    if (!properties_wave_ && !properties_stream_)
    {
        return;
    }

    // On the Wii, a wave's wave archive is the wave data of a bank or of a wave sound file, by its file's ID.
    const format::File* document = Document();
    const bool wii = wii_archive_ && properties_wave_;
    const bool wii_stream = wii_archive_ && properties_stream_ && *properties_stream_ < wii_archive_->Sounds().size();
    const bool listed =
        wii || wii_stream ||
        (archive_ && (properties_wave_ ? properties_wave_->wave_archive < archive_->WaveArchives().size()
                                       : *properties_stream_ < archive_->Sounds().size()));
    if (document == nullptr || !archive_version_ || !listed)
    {
        CloseProperties();
        return;
    }

    // The view follows each archive that the program builds, and each move in the history back to the archive that the
    // program holds (FollowHistory). A version without the wave, such as the one before the import that added it,
    // closes the window too. On the Wii, so does a version without the wave's wave data.
    if (wave_view_version_ != archive_version_)
    {
        try
        {
            if (wii &&
                std::ranges::none_of(song::WaveArchivesOf(*wii_archive_), [this](const song::WiiWaveArchive& each)
                                     { return each.file_id == properties_wave_->wave_archive; }))
            {
                CloseProperties();
                return;
            }
            if (properties_wave_ &&
                properties_wave_->wave >= (wii ? song::WaveCountOf(*wii_archive_, properties_wave_->wave_archive)
                                               : song::WaveCountOf(*archive_, properties_wave_->wave_archive)))
            {
                CloseProperties();
                return;
            }

            wave_view_ = wii          ? MakeWaveView(*archive_version_, *wii_archive_, properties_wave_->wave_archive,
                                                     properties_wave_->wave)
                         : wii_stream ? MakeStreamView(*archive_version_, *wii_archive_, *properties_stream_)
                         : properties_wave_ ? MakeWaveView(*archive_version_, *archive_, properties_wave_->wave_archive,
                                                           properties_wave_->wave)
                                            : MakeStreamView(*archive_version_, *archive_, *properties_stream_);
            wave_view_version_ = archive_version_;
            if (wii)
            {
                MeasureGroups(wave_group_sizes_, properties_wave_->wave_archive);
            }
            else if (properties_wave_)
            {
                MeasureGroups(wave_group_sizes_, archive_->WaveArchives()[properties_wave_->wave_archive].file_id);
            }
        }
        catch (const std::exception& error)
        {
            ShowMessage(
                std::string(properties_wave_ ? "Hokuto can't show this wave. " : "Hokuto can't show this stream. ") +
                format::AsSentence(error.what()));
            CloseProperties();
            return;
        }
    }

    // While the program builds the archive of another version, or another window waits, the window takes no edits. Play
    // shows as Stop until the item has played to its end.
    const bool busy = EditorsWait();
    if (wave_state_.playing && !worker_.Playing() && ring_.Available() == 0)
    {
        wave_state_.playing = false;
    }
    TakeGroupSizes(wave_group_sizes_, wave_view_->sizes);
    bool open = true;
    const WaveEdit wave_edit = DrawWaveWindow(*wave_view_, wave_state_, busy, &open);
    if (!open)
    {
        CloseProperties();
        return;
    }

    ApplyWaveEdit(wave_edit);
}

void App::ApplyWaveEdit(const WaveEdit& wave_edit)
{
    if (!wave_edit.refused.empty())
    {
        ShowMessage(wave_edit.refused);
    }
    if (wave_edit.play)
    {
        PlayProperties();
    }
    if (wave_edit.stop)
    {
        worker_.Stop();
        wave_state_.playing = false;
    }
    if (wave_edit.replace || wave_edit.export_item)
    {
        if (const std::optional<BrowserPlace> place = PropertiesPlace())
        {
            wave_edit.replace ? StartReplace(*place) : StartItemExport(*place);
        }
    }
    if (!wave_edit.values || !wave_view_)
    {
        return;
    }

    // A wave takes the values through its model, and a stream through its file. Without a loop, the loop starts at 0.
    const WaveValues& values = *wave_edit.values;
    try
    {
        const format::File& document = *Document();
        format::File file;
        if (properties_wave_)
        {
            const uint32_t wave_archive = properties_wave_->wave_archive;
            const uint32_t wave = properties_wave_->wave;
            format::WaveModel model = wii_archive_ ? edit::WaveModelOf(document, *wii_archive_, wave_archive, wave)
                                                   : edit::WaveModelOf(document, *archive_, wave_archive, wave);
            model.encoding = values.encoding;
            model.rate = values.rate;
            model.loop = values.loop;
            model.loop_start = values.loop ? values.loop_start : 0;
            model.loop_end = values.loop_end;
            file = wii_archive_ ? edit::WithWaveModel(document, *wii_archive_, wave_archive, wave, std::move(model))
                                : edit::WithWaveModel(document, *archive_, wave_archive, wave, std::move(model));
        }
        else
        {
            const uint32_t sound = *properties_stream_;
            const std::vector<uint8_t>& bytes =
                wii_archive_ ? StreamBytes(document, *wii_archive_, sound) : StreamBytes(document, *archive_, sound);
            edit::StreamValues stream = edit::StreamValuesOf(bytes);
            stream.encoding = values.encoding;
            stream.rate = values.rate;
            stream.loop = values.loop;
            stream.loop_start = values.loop ? values.loop_start : 0;
            stream.length = values.loop_end;
            std::vector<uint8_t> made = edit::StreamWithValues(bytes, stream);
            const uint32_t id =
                wii_archive_ ? *song::StreamFileOf(*wii_archive_, sound) : *song::StreamFileOf(*archive_, sound);
            file = edit::ReplaceStream(document, id, std::move(made));
        }

        // The window shows the new values at once. The view follows the version's archive once the program has built
        // it.
        const format::File* before = Document();
        Do(wave_edit.name, wave_view_->name, std::move(file));
        if (Document() != before)
        {
            wave_view_->values = values;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::PlayProperties()
{
    if (!wave_view_ || wave_view_->samples.empty())
    {
        return;
    }

    // The item plays as the window shows it: up to its end, then round its loop until Stop.
    ntdlib::sar::Pcm pcm;
    pcm.sample_rate = wave_view_->values.rate;
    pcm.loop = wave_view_->values.loop;
    pcm.loop_start = wave_view_->values.loop_start;
    pcm.loop_end = std::min(wave_view_->values.loop_end, wave_view_->held);
    pcm.channels = wave_view_->samples;
    try
    {
        PlayAlone(std::make_unique<song::WavePreview>(std::move(pcm), true));
        wave_state_.playing = true;
    }
    catch (const std::exception& error)
    {
        ShowMessage(
            std::string(wave_view_->stream ? "Hokuto can't play this stream. " : "Hokuto can't play this wave. ") +
            format::AsSentence(error.what()));
    }
}

void App::ApplyWaveSoundChange(SoundSection& section)
{
    const std::optional<WaveSoundChange> change = std::exchange(section.wave_changed, std::nullopt);
    const format::File* document = Document();
    if (!change || document == nullptr || !section.wave_sound || !section.wave_sound_of || EditorsWait())
    {
        return;
    }

    // The section shows the new values at once. The view follows the version's archive once the program has built it.
    try
    {
        format::File file = wii_archive_ ? edit::WithWaveSoundFields(*document, *wii_archive_, *section.wave_sound_of,
                                                                     change->values, change->wave)
                                         : edit::WithWaveSoundFields(*document, *archive_, *section.wave_sound_of,
                                                                     change->values, change->wave);
        Do(change->name, change->details, std::move(file));
        if (Document() != document)
        {
            section.wave_sound->fields.values = change->values;
            section.wave_sound->fields.wave = change->wave;
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ShowWaveSoundIn(SoundSection& section)
{
    // For a new view, a thread measures the groups that load the wave sound file. Their lines join the view once
    // they're ready.
    if (wii_archive_)
    {
        if (ShowWaveSound(section, archive_version_, archive_version_ ? wii_archive_.get() : nullptr, EditorsWait()) &&
            section.wave_sound)
        {
            MeasureGroups(section.wave_group_sizes, wii_archive_->Sounds()[*section.wave_sound_of].file_id);
        }
    }
    else if (ShowWaveSound(section, archive_version_, archive_version_ ? archive_.get() : nullptr, EditorsWait()) &&
             section.wave_sound)
    {
        MeasureGroups(section.wave_group_sizes, archive_->Sounds()[*section.wave_sound_of].file_id);
    }
    if (section.wave_sound)
    {
        TakeGroupSizes(section.wave_group_sizes, section.wave_sound->sizes);
    }
    else
    {
        // A section without a wave sound's view, such as one that shows a sound of another kind, drops the measure of
        // its last wave sound.
        DropGroupSizes(section.wave_group_sizes);
    }
}

bool App::EditorsWait() const
{
    const format::File* document = Document();
    return document == nullptr || (!archive_ && !wii_archive_) || !archive_version_ || reading_version_ != nullptr ||
           !SameArchive(*document, *archive_version_) || replace_ || export_.has_value();
}

void App::MeasureGroups(std::future<std::vector<std::string>>& sizes, uint32_t file_id)
{
    // The version and its archive stay as they are. A group that can't be measured gives no line.
    const auto measure = [version = archive_version_, archive = archive_, wii = wii_archive_ != nullptr, file_id]
    {
        try
        {
            return wii ? WiiGroupSizeLines(*version, file_id) : GroupSizeLines(*version, *archive, file_id);
        }
        catch (const std::exception&)
        {
            return std::vector<std::string>();
        }
    };
    DropGroupSizes(sizes);
    sizes = std::async(std::launch::async, measure);
}

void App::DropGroupSizes(std::future<std::vector<std::string>>& sizes)
{
    if (sizes.valid() && !Done(sizes))
    {
        dropped_group_sizes_.push_back(std::move(sizes));
    }
    sizes = {};
}

std::optional<BrowserPlace> App::PropertiesPlace() const
{
    // A wave's row lies under its wave archive's, and a stream sound's among the sounds.
    const std::size_t group = properties_wave_ ? kWavesGroup : kSoundsGroup;
    const uint32_t number = properties_wave_ ? properties_wave_->wave_archive : properties_stream_.value_or(0);
    const std::vector<uint32_t>& items = browser_items_[group];
    for (std::size_t item = 0; item < items.size(); ++item)
    {
        if (items[item] != number)
        {
            continue;
        }

        if (!properties_wave_)
        {
            return BrowserPlace{static_cast<int>(group), static_cast<int>(item), -1};
        }

        const std::vector<uint32_t>& children = browser_children_[group][item];
        const auto found = std::ranges::find(children, properties_wave_->wave);
        if (found != children.end())
        {
            return BrowserPlace{static_cast<int>(group), static_cast<int>(item),
                                static_cast<int>(found - children.begin())};
        }
    }

    return std::nullopt;
}

void App::Do(std::string name, std::string details, format::File file, std::optional<KeySelection> after,
             std::vector<uint32_t> replaced)
{
    // Each timeline that the edit changed takes the converter's layout, and the selections follow its commands. The
    // replaced sequences and the new ones take the mode that the program finds for them.
    const format::File edited = file;
    edit::Settled settled;
    try
    {
        settled = edit::SettleTimelines(*Document(), std::move(file), replaced);
    }
    catch (const std::invalid_argument& error)
    {
        ShowMessage(error.what());
        return;
    }

    after = SettledSelection(std::move(after), edited, settled);
    list_selection_ = SettledSelection(std::move(list_selection_), edited, settled);
    control_selection_ = SettledSelection(std::move(control_selection_), edited, settled);
    file = settled.replaced.empty() ? std::move(settled.file)
                                    : edit::WithNewModes(std::move(settled.file), settled.replaced);

    // An edit that can change what a sound plays drops the lengths for players. hok lengths measures them again.
    file = format::WithoutStaleLengths(*Document(), std::move(file));

    // The Undo History's rows follow the actions, and so do the editors' selections around them. A new action drops the
    // rows of those that were undone. An edit in the List Editor leaves its selection in `list_selection_`.
    const auto now = std::chrono::system_clock::now();
    std::optional<KeySelection> before;
    if (key_editor_open_)
    {
        before = KeySelection{key_sequence_, key_editor_track_, key_editor_.selected};
    }
    std::optional<KeySelection> list_before;
    if (list_editor_open_)
    {
        list_before = KeySelection{list_sequence_, list_editor_track_, list_editor_.selected};
    }
    history_rows_.resize(history_->Done());
    history_rows_.push_back({name, details, TimeOfDay(now)});
    action_selections_.resize(history_->Done());
    action_selections_.push_back({std::move(before), after, std::move(list_before), list_selection_});
    key_selection_ = std::move(after);
    history_->Do({std::move(name), std::move(details), now, std::make_shared<const format::File>(std::move(file))});
    FollowVersion();
}

void App::ChangeEffects(const ActionNames& names, const BusEffects& effects)
{
    format::File file = *Document();
    file.aux_effects = effects;
    Do(names.name, names.details, std::move(file));
}

void App::FollowVersion()
{
    UpdateTitle();

    // The sequence plays on a new model with the version's effects, in the sound's mixing mode. The new model runs up
    // to the player's frame, and the player then takes it. While playback is stopped, the model runs through the first
    // pass for the next seek.
    const BusEffects* effects = Effects();
    const song::Mixing mixing = ShownMixing();
    if (sequence_ && effects != nullptr && (*effects != model_effects_ || mixing != model_mixing_))
    {
        model_effects_ = *effects;
        model_mixing_ = mixing;
        StartCatchUp();
    }

    // A version whose archive differs from the one that the zones show, or that the reader builds, builds its archive.
    // So does a version that gives the shown sound another engine.
    const format::File* document = Document();
    const format::File* target = reading_version_ ? reading_version_.get() : archive_version_.get();
    if ((archive_ || wii_archive_) && document != nullptr && target != nullptr &&
        (!SameArchive(*document, *target) || (sequence_ && !EngineServes(*document, sequence_->sound))))
    {
        ReadShown();
    }
}

void App::FollowHistory(std::size_t from, const format::File& left)
{
    // The selections wait for the version, until the editors show it.
    const std::size_t done = history_->Done();
    key_selection_.reset();
    list_selection_.reset();
    if (done < from && done < action_selections_.size())
    {
        key_selection_ = action_selections_[done].before;
        list_selection_ = action_selections_[done].list_before;
    }
    else if (done > from && done <= action_selections_.size())
    {
        key_selection_ = action_selections_[done - 1].after;
        list_selection_ = action_selections_[done - 1].list_after;
    }

    // The drags of the Bank Editor and the properties, the sliders of the Sound sections' wave sounds and the mixer's
    // controls hold values of the version that the history left. A track's edit selects its new track once its version
    // is read. A move to a version whose archive differs from `left`'s leaves those values stale. The new track then
    // belongs to a version that no longer shows. An Undo of an FX fader keeps the archive, and so does the drop of an
    // engine edit whose conversion failed. Such a move leaves them all.
    const format::File* document = Document();
    if (document != nullptr && !SameArchive(left, *document))
    {
        bank_state_.dragging.reset();
        bank_state_.held.reset();
        wave_state_.dragging.reset();
        wave_state_.held.reset();
        sound_section_.wave_held.reset();
        browser_sound_.wave_held.reset();
        track_to_select_.reset();
        if (song_)
        {
            for (TrackView& track : song_->tracks)
            {
                track.held.reset();
            }
        }
    }

    // The effect's window keeps its draft while the user drags one of its sliders. After a move to other effects, the
    // draft takes the version's effect. The slider that the user still holds then writes its value into it.
    if (document != nullptr && left.aux_effects != document->aux_effects)
    {
        effect_changing_ = false;
    }

    FollowVersion();

    // The Bank Editor, the properties and the Sound sections show an item's edit before its version's archive is built.
    // Each view is made again when the program's archive changes. An Undo during that build can lead back to the
    // version whose archive the program holds. No build or read then comes, and the views are made again from that
    // archive. A move to another archive builds it, and the views follow once it's built. A move that keeps the archive
    // leaves the views as they are.
    if (document != nullptr && archive_version_ && SameArchive(*document, *archive_version_) &&
        !SameArchive(left, *archive_version_))
    {
        bank_view_version_.reset();
        wave_view_version_.reset();
        sound_section_.wave_version.reset();
        browser_sound_.wave_version.reset();
    }
}

void App::OpenBrowsed()
{
    // A click on a bank's instrument, a wave archive's wave, a wave sound or a stream sound plays it, or stops it while
    // it plays. A preview that has played to its end no longer shows as playing.
    if (std::exchange(browser_state_.clicked, false))
    {
        if (browser_state_.playing == browser_state_.selected)
        {
            worker_.Stop();
            browser_state_.playing.reset();
        }
        else
        {
            PlayPreview(browser_state_.selected);
        }
    }
    if (browser_state_.playing && !worker_.Playing() && ring_.Available() == 0)
    {
        browser_state_.playing.reset();
    }

    // A sequence chosen in the browser opens. The other items don't show in the zones yet.
    if (browser_state_.selected.group == shown_group_ && browser_state_.selected.item == shown_item_)
    {
        return;
    }

    // The zones go on showing the last sequence until the next one has been read. Another choice abandons the read.
    shown_group_ = browser_state_.selected.group;
    shown_item_ = browser_state_.selected.item;
    ReadShown();
}

void App::ReadShown()
{
    // A request without a conversion converts nothing (ConvertFor).
    converting_engine_.reset();
    converting_version_.reset();
    const auto& sequences = browser_items_[kSequencesGroup];
    const std::shared_ptr<const format::File> version = history_ ? history_->Current() : nullptr;
    const bool browsed = shown_group_ == static_cast<int>(kSequencesGroup) && shown_item_ >= 0 &&
                         shown_item_ < static_cast<int>(sequences.size());

    // A version without the sound, such as the one before the import that added it, closes the zones' sequence, on the
    // Wii too. The browser then lists the version's items.
    if ((archive_ || wii_archive_) && version && version->native && version->native->tables && archive_version_ &&
        (browsed || sequence_))
    {
        const uint32_t sound = browsed ? sequences[static_cast<std::size_t>(shown_item_)] : sequence_->sound;
        if (sound >= version->native->tables->sounds.size())
        {
            CloseSequence();
            BuildArchive(version);
            return;
        }
    }

    // A sequence of the Wii reads from the open archive when it serves the version, and from the version's archive
    // otherwise. A sound on another console's engine plays its conversion for that console. The reader makes the
    // conversion too.
    if (wii_archive_ && version && version->native && archive_version_ && (browsed || sequence_))
    {
        const uint32_t sound = browsed ? sequences[static_cast<std::size_t>(shown_item_)] : sequence_->sound;
        song::StartValues values = StartValuesOf(sound);
        song::Changes changes = ChangesOf(sound);
        const ntdlib::SndBuild snd_build = format::BuildOfWiiArchive(*version->native);
        if (!SameArchive(*version, *archive_version_))
        {
            reading_version_ = version;
            const auto build = [version, name = file_name_]
            {
                return song::LoadWiiArchive(format::ExportArchive(*version), name, ntdlib::rvl::snd::kRsarOptions);
            };
            reader_.Request(song::SequenceReader::WiiBuild(build), sound, std::move(values), std::move(changes),
                            snd_build, ConvertFor(version, sound));
            return;
        }

        reading_version_.reset();
        if (sequence_ && sequence_->sound == sound && sequence_->start_values == values &&
            sequence_->changes == changes && sequence_->wii_build == snd_build && EngineServes(*version, sound))
        {
            reader_.Abandon();
        }
        else
        {
            reader_.Request(wii_archive_, sound, std::move(values), std::move(changes), snd_build,
                            ConvertFor(version, sound));
        }
        return;
    }

    if (!archive_ || !version || !archive_version_ || (!browsed && !sequence_))
    {
        reader_.Abandon();
        reading_version_.reset();
        if ((archive_ || wii_archive_) && version && version->native && archive_version_ &&
            !SameArchive(*version, *archive_version_))
        {
            BuildArchive(version);
        }
        return;
    }

    // The open archive serves the version that it came from. While the browser shows another kind of item, the zones go
    // on showing their sequence. The variables start with the values that the user chose, and change as the user asked.
    const uint32_t sound = browsed ? sequences[static_cast<std::size_t>(shown_item_)] : sequence_->sound;
    song::StartValues values = StartValuesOf(sound);
    song::Changes changes = ChangesOf(sound);

    // A sound on another console's engine plays its conversion for that console. The reader makes the conversion too.
    if (SameArchive(*version, *archive_version_))
    {
        reading_version_.reset();
        if (sequence_ && sequence_->sound == sound && sequence_->start_values == values &&
            sequence_->changes == changes && EngineServes(*version, sound))
        {
            reader_.Abandon();
        }
        else
        {
            reader_.Request(archive_, sound, std::move(values), std::move(changes), ConvertFor(version, sound));
        }
        return;
    }

    // Another version builds its archive, with the group files that the file keeps.
    reading_version_ = version;
    const std::string name = file_name_;
    const auto build = [version, name]
    {
        return song::LoadArchive(format::ExportArchive(*version), name, format::ExportGroupFiles(*version));
    };
    reader_.Request(build, sound, std::move(values), std::move(changes), ConvertFor(version, sound));
}

void App::BuildArchive(const std::shared_ptr<const format::File>& version)
{
    try
    {
        if (wii_archive_)
        {
            wii_archive_ =
                song::LoadWiiArchive(format::ExportArchive(*version), file_name_, ntdlib::rvl::snd::kRsarOptions);
            archive_version_ = version;
            FollowItems();
            return;
        }

        archive_ = song::LoadArchive(format::ExportArchive(*version), file_name_, format::ExportGroupFiles(*version));
        archive_version_ = version;
        FollowItems();
    }
    catch (const std::exception& error)
    {
        ShowMessage(std::string("Hokuto can't build this version's archive. ") + format::AsSentence(error.what()));
    }
}

void App::ListBrowser()
{
    for (std::size_t group = 0; group < browser_.size(); group++)
    {
        browser_[group].items.clear();
        browser_items_[group].clear();
        browser_children_[group].clear();
    }

    if (wii_archive_)
    {
        ListWiiBrowser();
        return;
    }

    // Every item exports but a stream sound whose stream the file lacks, and Export… says why. A wave archive's waves
    // export too, and so do a wave sound file's wave sounds.
    const auto add = [this](std::size_t group, const std::string& item_name, std::size_t index,
                            std::vector<uint32_t> children, bool exports)
    {
        BrowserItem item;
        item.name = item_name;
        for (const uint32_t child : children)
        {
            item.children.push_back(group == kWaveSoundFilesGroup ? archive_->Sounds()[child].name
                                                                  : ChildName(group, child));
        }
        item.exports = exports;
        item.children_export = group == kBanksGroup || group == kWavesGroup || group == kWaveSoundFilesGroup;
        browser_[group].items.push_back(std::move(item));
        browser_items_[group].push_back(static_cast<uint32_t>(index));
        browser_children_[group].push_back(std::move(children));
    };

    // A sequence sound's sequence can be replaced when its file holds one of the file's sequences, and the tables say
    // where the sound starts. A wave sound plays on a click. So does a stream sound: a message says why one whose
    // stream the file lacks can't play. A file can take the place of a bank's, a wave archive's or a wave sound's file,
    // and a WAV file the place of a wave. A stream file or a WAV file can take the place of a stream sound's stream, a
    // file beside the archive.
    const format::File& version = *history_->Current();
    const bool native = static_cast<bool>(version.native);
    const std::vector<std::optional<edit::SoundPlace>> places =
        start_fields_ ? edit::SoundPlaces(*archive_, file_sequences_, *start_fields_)
                      : std::vector<std::optional<edit::SoundPlace>>{};
    for (std::size_t i = 0; i < archive_->Sounds().size(); i++)
    {
        const ntdlib::sar::SoundInfo& sound = archive_->Sounds()[i];
        const bool sequence = sound.type == ntdlib::sar::SoundType::kSequence;
        const bool wave_sound = sound.type == ntdlib::sar::SoundType::kWaveSound;
        const std::optional<uint32_t> stream =
            native ? song::StreamFileOf(*archive_, static_cast<uint32_t>(i)) : std::nullopt;
        const bool kept = stream && version.native->streams.contains(*stream);
        const bool aac = kept && song::IsAdts(*version.native->streams.at(*stream).bytes);
        const std::size_t group = sequence ? kSequencesGroup : kSoundsGroup;
        add(group, sound.name, i, {}, sequence || wave_sound || kept);
        BrowserItem& item = browser_[group].items.back();
        item.replaces = native && ((i < places.size() && places[i]) || wave_sound || stream.has_value());
        item.plays = wave_sound || (native && sound.type == ntdlib::sar::SoundType::kStream);
        if (kept)
        {
            item.edit = kStreamProperties;
            item.edits = !aac;
            item.edit_note = aac ? "Hokuto shows no properties of an AAC stream. Replace… with a WAV file sets the "
                                   "stream's loop from the WAV file's loop."
                                 : "";
        }
        if (sound.type == ntdlib::sar::SoundType::kStream && !aac)
        {
            item.export_note = MissingStreamNote(song::StreamFileName(*archive_, static_cast<uint32_t>(i)));
        }
        if (sound.type == ntdlib::sar::SoundType::kStream && native && !stream)
        {
            item.replace_note =
                "The archive gives the stream no path in its folder. Hokuto replaces only the streams there.";
        }
    }

    // A bank lists its instruments by their programs, and a wave archive its waves. A bank or a wave archive that can't
    // be read lists none.
    for (std::size_t i = 0; i < archive_->Banks().size(); i++)
    {
        add(kBanksGroup, archive_->Banks()[i].name, i, ChildrenOf(*archive_, kBanksGroup, i), true);
        browser_[kBanksGroup].items.back().replaces = native;
        if (native)
        {
            browser_[kBanksGroup].items.back().edit = kOpenBankEditor;
            browser_[kBanksGroup].items.back().children_edit = kOpenBankEditor;
        }
    }
    for (std::size_t i = 0; i < archive_->WaveArchives().size(); i++)
    {
        add(kWavesGroup, archive_->WaveArchives()[i].name, i, ChildrenOf(*archive_, kWavesGroup, i), true);
        browser_[kWavesGroup].items.back().replaces = native;
        browser_[kWavesGroup].items.back().children_replace = native;
        if (native)
        {
            browser_[kWavesGroup].items.back().children_edit = kWaveProperties;
            browser_[kWavesGroup].items.back().children_drag = true;
        }
    }

    // A wave sound file lists its wave sounds, and goes by its first. A file can take its place.
    for (const song::WaveSoundSet& set : song::WaveSoundSets(*archive_))
    {
        add(kWaveSoundFilesGroup, set.name, set.sounds.front(), set.sounds, true);
        browser_[kWaveSoundFilesGroup].items.back().replaces = native;
        browser_[kWaveSoundFilesGroup].items.back().children_replace = native;
        browser_[kWaveSoundFilesGroup].items.back().children_found = true;
    }

    // An item whose file lies in a group file that the Hokuto file lacks shows grey, and says why. Its commands then
    // wait for a new import of the archive with the group file beside it.
    const std::map<uint32_t, std::vector<std::string>> missing = song::FilesInMissingGroups(*archive_);
    if (missing.empty())
    {
        return;
    }

    const auto file_of = [this](std::size_t group, uint32_t index) -> std::optional<uint32_t>
    {
        if (group == kBanksGroup)
        {
            return archive_->Banks()[index].file_id;
        }
        if (group == kWavesGroup)
        {
            return archive_->WaveArchives()[index].file_id;
        }

        const ntdlib::sar::SoundInfo& sound = archive_->Sounds()[index];
        return sound.type == ntdlib::sar::SoundType::kStream ? std::nullopt : std::optional<uint32_t>(sound.file_id);
    };
    for (const std::size_t group : {kSequencesGroup, kSoundsGroup, kBanksGroup, kWavesGroup, kWaveSoundFilesGroup})
    {
        for (std::size_t i = 0; i < browser_[group].items.size(); i++)
        {
            const std::optional<uint32_t> file = file_of(group, browser_items_[group][i]);
            const auto found = file ? missing.find(*file) : missing.end();
            if (found != missing.end())
            {
                BrowserItem& item = browser_[group].items[i];
                item.missing = song::MissingGroupNote(item.name, found->second);
                item.plays = false;
            }
        }
    }
}

void App::ListWiiBrowser()
{
    // The wave data of the banks and of the wave sound files lists as wave archives, by their files' IDs
    // (song::WaveArchivesOf). A bank or a wave archive that can't be read lists no children. Every item exports but a
    // stream sound whose stream the file lacks, and Export… says why. A wave sound file's wave sounds export too, and
    // so do a wave archive's waves. A wave sound and a stream sound play on a click, and a stream sound's stream opens
    // in its properties. A sequence sound's sequence, a bank's file, a wave sound's file and a wave archive that is an
    // RWAR take a file of the Wii in their place. A wave takes a WAV file or a wave file of the Wii, and a stream
    // sound's stream a stream file or a WAV file.
    const ntdlib::rsar::SoundArchive& archive = *wii_archive_;
    const format::File& version = *history_->Current();
    const bool replaces = static_cast<bool>(version.native);
    const std::vector<std::optional<edit::SoundPlace>> places =
        start_fields_ ? edit::SoundPlaces(archive, file_sequences_, *start_fields_)
                      : std::vector<std::optional<edit::SoundPlace>>{};

    const auto add = [this](std::size_t group, const std::string& item_name, std::size_t index,
                            std::vector<uint32_t> children, std::vector<std::string> child_names, bool exports)
    {
        BrowserItem item;
        item.name = item_name;
        item.children = std::move(child_names);
        item.exports = exports;
        item.children_export = group == kBanksGroup || group == kWavesGroup || group == kWaveSoundFilesGroup;
        browser_[group].items.push_back(std::move(item));
        browser_items_[group].push_back(static_cast<uint32_t>(index));
        browser_children_[group].push_back(std::move(children));
    };

    const auto names = [](std::size_t group, const std::vector<uint32_t>& children)
    {
        std::vector<std::string> child_names;
        for (const uint32_t child : children)
        {
            child_names.push_back(ChildName(group, child));
        }

        return child_names;
    };

    for (std::size_t i = 0; i < archive.Sounds().size(); i++)
    {
        const ntdlib::rsar::SoundInfo& sound = archive.Sounds()[i];
        const bool sequence = sound.type == ntdlib::rsar::SoundType::kSequence;
        const bool wave_sound = sound.type == ntdlib::rsar::SoundType::kWaveSound;
        const std::optional<uint32_t> stream =
            version.native ? song::StreamFileOf(archive, static_cast<uint32_t>(i)) : std::nullopt;
        const bool kept = stream && version.native->streams.contains(*stream);
        const std::size_t group = sequence ? kSequencesGroup : kSoundsGroup;
        add(group, sound.name, i, {}, {}, sequence || wave_sound || kept);
        BrowserItem& item = browser_[group].items.back();
        item.replaces = replaces && ((i < places.size() && places[i]) || wave_sound || stream.has_value());
        item.plays = wave_sound || (version.native && sound.type == ntdlib::rsar::SoundType::kStream);
        if (kept)
        {
            item.edit = kStreamProperties;
        }
        else if (sound.type == ntdlib::rsar::SoundType::kStream)
        {
            item.export_note = MissingStreamNote(song::StreamFileName(archive, static_cast<uint32_t>(i)));
        }
    }
    for (std::size_t i = 0; i < archive.Banks().size(); i++)
    {
        std::vector<uint32_t> programs;
        try
        {
            programs = song::ProgramsOf(archive, i);
        }
        catch (const std::exception&)
        {
            programs.clear();
        }
        add(kBanksGroup, archive.Banks()[i].name, i, programs, names(kBanksGroup, programs), true);
        browser_[kBanksGroup].items.back().replaces = replaces;
    }
    for (const song::WiiWaveArchive& wave_archive : song::WaveArchivesOf(archive))
    {
        std::vector<uint32_t> waves;
        try
        {
            waves.resize(song::WaveCountOf(archive, wave_archive.file_id));
            std::iota(waves.begin(), waves.end(), 0u);
        }
        catch (const std::exception&)
        {
            waves.clear();
        }
        add(kWavesGroup, wave_archive.name, wave_archive.file_id, waves, names(kWavesGroup, waves), true);
        browser_[kWavesGroup].items.back().replaces = replaces;
        browser_[kWavesGroup].items.back().children_replace = replaces;
    }
    for (const song::WaveSoundSet& set : song::WaveSoundSets(archive))
    {
        std::vector<std::string> sounds;
        for (const uint32_t sound : set.sounds)
        {
            sounds.push_back(archive.Sounds()[sound].name);
        }
        add(kWaveSoundFilesGroup, set.name, set.sounds.front(), set.sounds, std::move(sounds), true);
        browser_[kWaveSoundFilesGroup].items.back().replaces = replaces;
        browser_[kWaveSoundFilesGroup].items.back().children_replace = replaces;
        browser_[kWaveSoundFilesGroup].items.back().children_found = true;
    }

    // A bank opens in the Bank Editor, and a wave archive's wave in its properties. A wave drags onto a bank.
    for (BrowserItem& item : browser_[kBanksGroup].items)
    {
        item.edit = kOpenBankEditor;
        item.children_edit = kOpenBankEditor;
    }
    for (BrowserItem& item : browser_[kWavesGroup].items)
    {
        item.children_edit = kWaveProperties;
        item.children_drag = true;
    }
}

void App::FollowItems()
{
    if (!archive_ && !wii_archive_)
    {
        return;
    }
    if (!archive_version_)
    {
        ListChildren();
        return;
    }

    const std::size_t sounds = browser_items_[kSoundsGroup].size() + browser_items_[kSequencesGroup].size();
    const bool same = wii_archive_
                          ? sounds == wii_archive_->Sounds().size() &&
                                browser_items_[kBanksGroup].size() == wii_archive_->Banks().size() &&
                                browser_items_[kWavesGroup].size() == song::WaveArchivesOf(*wii_archive_).size()
                          : sounds == archive_->Sounds().size() &&
                                browser_items_[kBanksGroup].size() == archive_->Banks().size() &&
                                browser_items_[kWavesGroup].size() == archive_->WaveArchives().size();

    // The places of the sequences and of the starts follow the version that the archive came from.
    if (!same)
    {
        try
        {
            const std::vector<uint8_t> bytes = format::ExportArchive(*archive_version_);
            file_sequences_ = wii_archive_ ? format::SequencesOfFiles(*archive_version_, bytes, *wii_archive_)
                                           : format::SequencesOfFiles(*archive_version_, bytes, *archive_);
            start_fields_ = StartFieldsOf(*archive_version_, bytes);
        }
        catch (const std::exception&)
        {
            file_sequences_.clear();
            start_fields_.reset();
        }
    }

    // The browser lists the items again. Its choices of items that went go too. Items go and come only at the ends of
    // the lists, and the others keep their places.
    ListBrowser();

    const auto listed = [this](int group, int item)
    {
        return group >= 0 && item >= 0 && static_cast<std::size_t>(group) < browser_items_.size() &&
               static_cast<std::size_t>(item) < browser_items_[static_cast<std::size_t>(group)].size();
    };
    if (!listed(browser_state_.selected.group, browser_state_.selected.item))
    {
        browser_state_.selected = {};
    }
    if (browser_state_.playing && !listed(browser_state_.playing->group, browser_state_.playing->item))
    {
        browser_state_.playing.reset();
    }
    std::erase_if(browser_state_.open,
                  [&listed](const std::pair<int, int>& open) { return !listed(open.first, open.second); });
    if (!listed(shown_group_, shown_item_))
    {
        shown_group_ = -1;
        shown_item_ = -1;
    }

    // The browser selects the sound that an import added, and scrolls to it. The zones then show it.
    if (const std::optional<uint32_t> sound = std::exchange(select_sound_, std::nullopt))
    {
        const std::vector<uint32_t>& sequences = browser_items_[kSequencesGroup];
        const auto found = std::ranges::find(sequences, *sound);
        if (found != sequences.end())
        {
            browser_state_.selected = {static_cast<int>(kSequencesGroup), static_cast<int>(found - sequences.begin())};
            browser_state_.reveal = true;
        }
    }
}

void App::CloseSequence()
{
    if (transport_.Playing())
    {
        transport_.Stop();
    }
    reader_.Abandon();
    reading_version_.reset();
    StopPlayback();
    CancelCatchUp();
    CloseKeyEditor();
    CloseListEditor();
    sequence_.reset();
    model_.reset();
    song_.reset();
    ForgetEngine();
    browser_state_.selected = {};
    shown_group_ = -1;
    shown_item_ = -1;
    arrangement_.selected_track = 0;
    arrangement_.selected_part_track = -1;
    arrangement_.selected_part = -1;
    transport_.SetCycle(false);
    transport_.SetPosition(0.0);
    transport_.TakeEmptySong();
}

void App::ListChildren()
{
    // On the Wii, a bank's children are the programs that play a region, and a wave archive's the waves of its file's
    // wave data. A bank or a wave archive that can't be read lists none.
    if (!archive_ && !wii_archive_)
    {
        return;
    }

    const auto children_of = [this](std::size_t group, uint32_t index)
    {
        if (!wii_archive_)
        {
            return ChildrenOf(*archive_, group, index);
        }

        std::vector<uint32_t> children;
        try
        {
            if (group == kBanksGroup)
            {
                children = song::ProgramsOf(*wii_archive_, index);
            }
            else
            {
                children.resize(song::WaveCountOf(*wii_archive_, index));
                std::iota(children.begin(), children.end(), 0u);
            }
        }
        catch (const std::exception&)
        {
            children.clear();
        }

        return children;
    };
    for (const std::size_t group : {kBanksGroup, kWavesGroup})
    {
        for (std::size_t i = 0; i < browser_items_[group].size(); i++)
        {
            std::vector<uint32_t> children = children_of(group, browser_items_[group][i]);
            std::vector<std::string>& names = browser_[group].items[i].children;
            names.clear();
            for (const uint32_t child : children)
            {
                names.push_back(ChildName(group, child));
            }
            browser_children_[group][i] = std::move(children);
        }
    }
}

void App::PlayPreview(const BrowserPlace& place)
{
    if (wii_archive_)
    {
        PlayWiiPreview(place);
        return;
    }

    // Only a bank's instruments, a wave archive's waves, the wave sounds and the stream sounds play by themselves. A
    // wave sound file's wave sounds play too.
    const auto group = static_cast<std::size_t>(place.group);
    if (!archive_ || place.group < 0 || place.item < 0 || group >= browser_items_.size() ||
        static_cast<std::size_t>(place.item) >= browser_items_[group].size())
    {
        return;
    }

    const auto item = static_cast<std::size_t>(place.item);
    const bool bank = group == kBanksGroup;
    const bool listed =
        place.child >= 0 && static_cast<std::size_t>(place.child) < browser_children_[group][item].size();
    const bool child = (bank || group == kWavesGroup) && listed;
    const uint32_t index = group == kWaveSoundFilesGroup && listed
                               ? browser_children_[group][item][static_cast<std::size_t>(place.child)]
                               : browser_items_[group][item];
    const bool sound = ((group == kSoundsGroup && place.child < 0) || (group == kWaveSoundFilesGroup && listed)) &&
                       index < archive_->Sounds().size();
    const bool wave_sound = sound && archive_->Sounds()[index].type == ntdlib::sar::SoundType::kWaveSound;
    const bool stream_sound =
        sound && archive_version_ && archive_->Sounds()[index].type == ntdlib::sar::SoundType::kStream;
    if (!child && !wave_sound && !stream_sound)
    {
        return;
    }

    // The bank's note, the wave or the sound's files are read before they play. A wave sound and a stream sound play on
    // the console's model. The model plays them as the game's sound library does. The file keeps the stream. An error
    // shows in a message box.
    std::unique_ptr<audio::Renderer> preview;
    try
    {
        // A sound plays in its mixing mode, and a bank's note in the file's.
        const format::File* document = Document();
        const format::Mixing file_mixing = document != nullptr ? document->mixing : format::Mixing::kStrict;
        const song::Mixing sound_mixing =
            edit::ModelMixing(document != nullptr && sound ? format::MixingOf(*document, index) : file_mixing);
        if (wave_sound)
        {
            preview = std::make_unique<song::SoundPreview>(archive_, index, std::vector<uint8_t>(), sound_mixing);
        }
        else if (stream_sound)
        {
            // ntdlib's players take an AAC stream as a stream file in 16-bit PCM, with the loop of the sound's entry.
            const std::vector<uint8_t>& stream = StreamBytes(*archive_version_, *archive_, index);
            preview = std::make_unique<song::SoundPreview>(
                archive_, index,
                song::IsAdts(stream)
                    ? song::PlayableStream(stream, song::AacStreamOf(*archive_, index).value_or(song::AacStream()))
                    : stream,
                sound_mixing);
        }
        else if (bank)
        {
            preview = std::make_unique<song::NotePreview>(
                archive_, index, browser_children_[group][item][static_cast<std::size_t>(place.child)],
                edit::ModelMixing(file_mixing));
        }
        else
        {
            preview = std::make_unique<song::WavePreview>(
                *archive_, index, browser_children_[group][item][static_cast<std::size_t>(place.child)]);
        }
    }
    catch (const std::exception& error)
    {
        const char* what = wave_sound     ? "Hokuto can't play this wave sound. "
                           : stream_sound ? "Hokuto can't play this stream sound. "
                           : bank         ? "Hokuto can't play this instrument. "
                                          : "Hokuto can't play this wave. ";
        ShowMessage(what + format::AsSentence(error.what()));
        return;
    }

    PlayAlone(std::move(preview));
    browser_state_.playing = place;
}

void App::PlayWiiPreview(const BrowserPlace& place)
{
    // On the Wii, a bank's instruments, a wave archive's waves, the wave sounds and the stream sounds play by
    // themselves. A wave sound file's wave sounds play too. A note, a wave sound and a stream sound play by the rules
    // of the file's build of NW4R SND. The file keeps a stream sound's stream.
    const auto group = static_cast<std::size_t>(place.group);
    if (place.group < 0 || place.item < 0 || group >= browser_items_.size() ||
        static_cast<std::size_t>(place.item) >= browser_items_[group].size())
    {
        return;
    }

    const auto item = static_cast<std::size_t>(place.item);
    const std::vector<uint32_t>& children = browser_children_[group][item];
    const bool listed = place.child >= 0 && static_cast<std::size_t>(place.child) < children.size();
    const bool bank = group == kBanksGroup && listed;
    const bool wave = group == kWavesGroup && listed;
    const uint32_t index = group == kWaveSoundFilesGroup && listed ? children[static_cast<std::size_t>(place.child)]
                                                                   : browser_items_[group][item];
    const bool sound = ((group == kSoundsGroup && place.child < 0) || (group == kWaveSoundFilesGroup && listed)) &&
                       index < wii_archive_->Sounds().size();
    const bool wave_sound = sound && wii_archive_->Sounds()[index].type == ntdlib::rsar::SoundType::kWaveSound;
    const bool stream_sound =
        sound && archive_version_ && wii_archive_->Sounds()[index].type == ntdlib::rsar::SoundType::kStream;
    if (!bank && !wave && !wave_sound && !stream_sound)
    {
        return;
    }

    std::unique_ptr<audio::Renderer> preview;
    try
    {
        const format::File* document = Document();
        std::optional<ntdlib::SndBuild> build;
        if (document != nullptr && document->native)
        {
            build = format::BuildOfWiiArchive(*document->native);
        }
        if (wave_sound)
        {
            preview = std::make_unique<song::SoundPreview>(wii_archive_, index, build);
        }
        else if (stream_sound)
        {
            preview = std::make_unique<song::SoundPreview>(wii_archive_, index,
                                                           StreamBytes(*archive_version_, *wii_archive_, index), build);
        }
        else if (bank)
        {
            preview = std::make_unique<song::NotePreview>(wii_archive_, index,
                                                          children[static_cast<std::size_t>(place.child)], build);
        }
        else
        {
            preview = std::make_unique<song::WavePreview>(
                song::DecodeItemWave(*wii_archive_, index, children[static_cast<std::size_t>(place.child)]));
        }
    }
    catch (const std::exception& error)
    {
        const char* what = wave_sound     ? "Hokuto can't play this wave sound. "
                           : stream_sound ? "Hokuto can't play this stream sound. "
                           : bank         ? "Hokuto can't play this instrument. "
                                          : "Hokuto can't play this wave. ";
        ShowMessage(what + format::AsSentence(error.what()));
        return;
    }

    PlayAlone(std::move(preview));
    browser_state_.playing = place;
}

void App::ShowSequence(song::SequenceReader::Result read)
{
    if (!read.sequence)
    {
        ShowMessage(read.error);
        return;
    }

    // The new sequence plays from its start if the last one was playing. It plays with the file's effects, in the
    // sound's mixing mode.
    StopPlayback();
    CancelCatchUp();
    CloseKeyEditor();
    CloseListEditor();
    sequence_ = std::move(read.sequence);
    const BusEffects* effects = Effects();
    model_effects_ = effects != nullptr ? *effects : BusEffects{};
    model_mixing_ = ShownMixing();
    model_ = MakeModel();
    model_engine_ = PlayedEngine();
    song_ = MakeSongView(*sequence_);
    song_->slots = archive_       ? SlotsOf(*archive_, sequence_->sound)
                   : wii_archive_ ? SlotsOf(*wii_archive_, sequence_->sound)
                                  : std::vector<SlotView>();

    // When a bank of the sound lies in a group file that the file lacks, a message says so the first time that the
    // sound shows while the file stays open. The browser shows the bank grey, but not the sound: its other notes can
    // play.
    if (archive_ && told_missing_banks_.insert(sequence_->sound).second)
    {
        const std::string note = song::MissingBanksNote(*archive_, sequence_->sound);
        if (!note.empty())
        {
            ShowMessage(note, MessageKind::kNote);
        }
    }

    arrangement_.selected_track = 0;
    arrangement_.selected_part_track = -1;
    arrangement_.selected_part = -1;

    // The transport takes the sequence's tempo. The locators go round the loop. Without one, they run from the start to
    // the beat on which the last note has ended. The release after it still plays, but a cycle leaves it out. A song
    // plays through once: Cycle starts off.
    const double quarter = sequence_->timebase;
    transport_.SetTempo(song::TempoAt(*sequence_, 0));
    transport_.SetBeatsPerBar(song_->beats_per_bar);
    if (sequence_->loop)
    {
        transport_.SetLocators(sequence_->loop->start / quarter, sequence_->loop->end / quarter);
    }
    else if (sequence_->notes_end > 0)
    {
        transport_.SetLocators(0.0, std::ceil(sequence_->notes_end / quarter));
    }
    else
    {
        transport_.SetLocators(0.0, sequence_->length / quarter);
    }
    transport_.SetCycle(false);
    transport_.SetPosition(0.0);
    ShowReach();
    ShowLogic();
    ShowOpeningInstruments();
    ShowTempo();
    ShowMode();

    // The first sound that shows after a file opens takes the file's view, if the view is the sound's.
    const std::optional<format::EditorView> view = std::exchange(pending_view_, std::nullopt);
    if (view && view->sound == sequence_->sound)
    {
        RestoreView(*view);
    }
}

std::optional<format::EditorView> App::CurrentView() const
{
    if (!sequence_ || !song_ || preview_)
    {
        return std::nullopt;
    }

    const LowerZone front = FrontOfLowerZone();
    const OpenEditors editors{key_editor_open_ ? std::optional<int>(key_editor_track_) : std::nullopt,
                              list_editor_open_ ? std::optional<int>(list_editor_track_) : std::nullopt,
                              front == LowerZone::kKeyEditor    ? format::ViewFront::kKeyEditor
                              : front == LowerZone::kListEditor ? format::ViewFront::kListEditor
                                                                : format::ViewFront::kMixer};
    return ViewOf(sequence_->sound, TrackNumbers(), transport_, arrangement_, *song_, key_editor_, editors);
}

void App::RestoreView(const format::EditorView& view)
{
    // The editors open on their tracks at the places that the view keeps.
    const OpenEditors editors = ApplyView(view, TrackNumbers(), transport_, arrangement_, *song_, key_editor_);
    if (editors.key_editor)
    {
        key_editor_open_ = true;
        key_editor_track_ = *editors.key_editor;
        key_editor_.selected.clear();
        key_editor_.gesture = KeyGesture();
        key_selection_.reset();
        UpdateKeyTrack();
    }
    if (editors.list_editor)
    {
        OpenCommands(*editors.list_editor, {}, 0);
    }
    lower_front_ = editors.front == format::ViewFront::kKeyEditor    ? LowerZone::kKeyEditor
                   : editors.front == format::ViewFront::kListEditor ? LowerZone::kListEditor
                                                                     : LowerZone::kMixer;
}

std::vector<int> App::TrackNumbers() const
{
    std::vector<int> numbers;
    if (sequence_)
    {
        for (const song::Track& track : sequence_->tracks)
        {
            numbers.push_back(track.number);
        }
    }

    return numbers;
}

void App::FollowSequence(song::SequenceReader::Result read)
{
    // Each track keeps its mute, its solo and its meters, by its number. The selection of a track and of a part's track
    // follows the track's number too. A track that a track's edit added becomes the selection.
    std::map<int, TrackView> kept;
    for (std::size_t i = 0; song_ && i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        kept.emplace(sequence_->tracks[i].number, song_->tracks[i]);
    }

    const auto number_at = [this](int index)
    {
        const bool valid = index >= 0 && static_cast<std::size_t>(index) < sequence_->tracks.size();
        return valid ? sequence_->tracks[static_cast<std::size_t>(index)].number : -1;
    };
    const int selected = std::exchange(track_to_select_, std::nullopt).value_or(number_at(arrangement_.selected_track));
    const int part_track = number_at(arrangement_.selected_part_track);
    sequence_ = std::move(read.sequence);
    song_ = MakeSongView(*sequence_);
    song_->slots = archive_       ? SlotsOf(*archive_, sequence_->sound)
                   : wii_archive_ ? SlotsOf(*wii_archive_, sequence_->sound)
                                  : std::vector<SlotView>();
    for (std::size_t i = 0; i < song_->tracks.size(); i++)
    {
        const auto found = kept.find(sequence_->tracks[i].number);
        if (found != kept.end())
        {
            TrackView& track = song_->tracks[i];
            track.mute = found->second.mute;
            track.solo = found->second.solo;
            track.meter = found->second.meter;
            track.peak = found->second.peak;
        }
    }

    const auto index_of = [this](int number)
    {
        const auto found = std::find_if(sequence_->tracks.begin(), sequence_->tracks.end(),
                                        [number](const song::Track& track) { return track.number == number; });
        return found != sequence_->tracks.end() ? static_cast<int>(found - sequence_->tracks.begin()) : -1;
    };
    arrangement_.selected_track = std::max(index_of(selected), 0);
    arrangement_.selected_part_track = index_of(part_track);
    if (arrangement_.selected_part_track < 0)
    {
        arrangement_.selected_part = -1;
    }

    ShowReach();
    ShowLogic();
    ShowOpeningInstruments();
    ShowTempo();
    ShowMode();

    StartCatchUp();

    // An editor whose track the new version lacks, such as after Remove Selected Tracks, shows the arrangement's
    // selected track instead. The zone in front stays in front.
    const LowerZone front = lower_front_;
    if (key_editor_open_ && TrackNumbered(key_editor_track_) == nullptr)
    {
        OpenKeyEditor();
    }
    if (list_editor_open_ && TrackNumbered(list_editor_track_) == nullptr)
    {
        OpenListEditor();
    }
    lower_front_ = front;
    UpdateKeyTrack();
    UpdateListTrack();
}

void App::ShowReach()
{
    // The edits go into the version that built the open archive, and the reach comes from it too. A sound that Hokuto
    // can't edit shows nothing.
    if (!song_ || !sequence_ || (!archive_ && !wii_archive_) || !archive_version_ || !start_fields_)
    {
        return;
    }

    std::vector<edit::TrackReach> reach;
    try
    {
        reach = edit::ReachOfTracks(*archive_version_, Places(), sequence_->sound, *sequence_);
    }
    catch (const std::exception&)
    {
        return;
    }

    const std::vector<std::string> names = SoundNames();
    for (std::size_t i = 0; i < reach.size() && i < song_->tracks.size(); i++)
    {
        TrackView& track = song_->tracks[i];
        for (std::size_t control = 0; control < kMixControlCount; control++)
        {
            const auto setting = static_cast<std::size_t>(MixOf(static_cast<MixControl>(control)));
            track.mix_reach[control] = ReachText(names, *sequence_, reach[i].mix[setting]);
        }
        track.instrument_reach = ReachText(names, *sequence_, reach[i].instrument);
    }
}

void App::ShowLogic()
{
    // The places come from the version that built the open archive. The List Editor's rows come from it too.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence_ || !sequence || !archive_version_)
    {
        return;
    }

    song_->logic = LogicMarkers(*archive_version_, *sequence, *sequence_);
}

void App::ShowOpeningInstruments()
{
    // The commands come from the version that built the open archive.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence_ || !sequence || !archive_version_)
    {
        return;
    }

    for (std::size_t i = 0; i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        const song::Track& track = sequence_->tracks[i];
        if (!track.notes.empty())
        {
            continue;
        }

        std::optional<edit::Program> opening;
        try
        {
            opening = edit::OpeningInstrument(*archive_version_, *sequence, track);
        }
        catch (const std::exception&)
        {
            continue;
        }

        if (opening && static_cast<std::size_t>(opening->slot) < song_->slots.size())
        {
            TrackView& view = song_->tracks[i];
            view.bank = song_->slots[static_cast<std::size_t>(opening->slot)].bank;
            view.program = opening->number;
        }
    }
}

void App::ShowMode()
{
    // The mode and the loop come from the version that built the open archive.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence || !archive_version_)
    {
        return;
    }

    const std::optional<edit::ConvertedLoop> loop = edit::TimelineLoop(*archive_version_, *sequence);
    song_->loop_moves = loop && song_->loop_start >= 0 && loop->start == static_cast<uint32_t>(song_->loop_start) &&
                        loop->end == static_cast<uint32_t>(song_->loop_end);
    song_->timeline = edit::IsTimeline(*archive_version_, *sequence);
    song_->timeline_refused.clear();
    if (!song_->timeline)
    {
        try
        {
            edit::AsTimeline(*archive_version_, *sequence);
        }
        catch (const std::exception& error)
        {
            song_->timeline_refused = error.what();
        }
    }
}

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
        *requested > static_cast<int>(format::Engine::kWii))
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

format::Engine App::PlayedEngine() const
{
    if (engine_sound_)
    {
        return engine_sound_->wii ? format::Engine::kWii
                                  : format::EngineOf(engine_sound_->archive->ReadOptions().flavor);
    }

    const format::File* document = Document();
    return document != nullptr ? document->engine : format::Engine::kSwitch;
}

std::shared_ptr<const ntdlib::sar::SoundArchive> App::PlayedArchive() const
{
    return engine_sound_ ? engine_sound_->archive : archive_;
}

std::shared_ptr<const ntdlib::rsar::SoundArchive> App::PlayedWiiArchive() const
{
    return engine_sound_ ? engine_sound_->wii : wii_archive_;
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
        if (engine == format::Engine::kWii)
        {
            song::SequenceReader::EngineSound wii{.warnings = std::move(converted.warnings)};
            wii.wii = song::LoadWiiArchive(format::ExportArchive(converted.file), name, ntdlib::rvl::snd::kRsarOptions);
            wii.wii_build = format::BuildOfWiiArchive(*converted.file.native);
            wii.slot_programs = std::move(converted.slot_programs);
            return wii;
        }

        return song::SequenceReader::EngineSound{
            .archive = song::LoadArchive(format::ExportArchive(converted.file), name),
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
    engine_version_ = engine ? version : nullptr;
    engine_error_ = std::move(read.engine_error);
    engine_failed_ = engine_error_.empty() ? std::nullopt : engine;
    const bool dropped = engine && DropEngineEdit(*engine, read.sequence->sound, !engine_error_.empty());
    const format::File* document = Document();
    if (!engine_error_.empty() && engine && (!known || dropped) && document != nullptr)
    {
        ShowMessage(std::string("Hokuto can't play the sound on the ") + EngineName(*engine) + "'s engine. " +
                    format::AsSentence(engine_error_) + " The sound " + (dropped ? "stays" : "plays") + " on the " +
                    EngineName(document->engine) + "'s engine.");
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
    engine_version_.reset();
    engine_error_.clear();
    engine_failed_.reset();
    converting_engine_.reset();
    converting_version_.reset();
}

void App::ShowTempo()
{
    // The points lead to the commands of the version that built the open archive. The List Editor's rows come from it
    // too.
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!song_ || !sequence_ || !sequence || !archive_version_)
    {
        return;
    }

    song_->tempo_points = TempoPoints(*archive_version_, *sequence, *sequence_);
    if ((!archive_ && !wii_archive_) || !start_fields_ || song_->tempo_points.empty())
    {
        return;
    }

    // The other sounds that an edit of each point's command changes. A sound that Hokuto can't edit lists none.
    std::vector<uint32_t> commands;
    for (const TempoPointView& point : song_->tempo_points)
    {
        commands.push_back(point.command);
    }
    std::vector<std::vector<uint32_t>> reaching;
    try
    {
        reaching = edit::SoundsReaching(*archive_version_, Places(), sequence_->sound, commands);
    }
    catch (const std::exception&)
    {
        return;
    }

    const std::vector<std::string> names = SoundNames();
    for (std::size_t i = 0; i < reaching.size() && i < song_->tempo_points.size(); i++)
    {
        song_->tempo_points[i].reach = ReachText(names, *sequence_, edit::Reach{{}, reaching[i]});
    }
}

void App::StartCatchUp()
{
    // A model of another engine runs in ticks, up to the sample that the device plays. While playback is stopped, it
    // runs through the first pass.
    if (PlayedEngine() != model_engine_)
    {
        engine_catch_up_ = PlayedEngine();
        const double behind =
            clock_ && playback_rate_ > 0 ? static_cast<double>(QueuedSamples()) / playback_rate_ : 0.0;
        catch_up_.Start(MakeModel(), clock_, true, behind);
        return;
    }

    // A model that plays the ticks at other times than the model that plays, such as after an edit of a tempo, takes
    // over at the player's position in ticks.
    engine_catch_up_.reset();
    const bool by_ticks = model_ != nullptr && !song::SameTiming(model_->GetSequence(), PlayedSequence());
    catch_up_.Start(MakeModel(), clock_, by_ticks);
}

void App::CancelCatchUp()
{
    catch_up_.Cancel();
    engine_catch_up_.reset();
}

std::shared_ptr<song::SequenceModel> App::MakeModel() const
{
    // On another console's engine, the model plays the sound's conversion. The model plays only the effects of its
    // console.
    if (engine_sound_ && engine_sequence_ && engine_sound_->wii)
    {
        return std::make_shared<song::SequenceModel>(engine_sound_->wii, engine_sequence_,
                                                     edit::ModelEffects(model_effects_));
    }
    if (engine_sound_ && engine_sequence_)
    {
        return std::make_shared<song::SequenceModel>(engine_sound_->archive, engine_sequence_,
                                                     edit::ModelEffects(model_effects_), model_mixing_);
    }
    if (wii_archive_)
    {
        return std::make_shared<song::SequenceModel>(wii_archive_, sequence_, edit::ModelEffects(model_effects_));
    }

    return std::make_shared<song::SequenceModel>(archive_, sequence_, edit::ModelEffects(model_effects_),
                                                 model_mixing_);
}

void App::OpenKeyEditor()
{
    // The selected part's track opens, or else the selected track. A track without notes has no parts.
    const std::optional<double> beat = std::exchange(arrangement_.open_beat, std::nullopt);
    const PartView* part = SelectedPart(song_, arrangement_);
    const int index = part != nullptr ? arrangement_.selected_part_track : arrangement_.selected_track;
    if (!sequence_ || index < 0 || static_cast<std::size_t>(index) >= sequence_->tracks.size())
    {
        return;
    }

    // The editor scrolls to the part's start, with the middle of the part's keys in the middle of the window. A track
    // opens at the beat of the double-click on its lane, or else at the position, with the middle of the track's keys
    // there. A track without notes opens round middle C.
    const auto by_key = [](const NoteView& a, const NoteView& b)
    {
        return a.key < b.key;
    };
    const auto by_note_key = [](const song::Note& a, const song::Note& b)
    {
        return a.key < b.key;
    };
    key_editor_open_ = true;
    lower_front_ = LowerZone::kKeyEditor;
    key_editor_track_ = sequence_->tracks[static_cast<std::size_t>(index)].number;
    key_editor_.selected.clear();
    key_editor_.gesture = KeyGesture();
    key_editor_.scroll_to.reset();
    const double quarters = beat.value_or(std::max(transport_.Position(), 0.0));
    key_editor_.scroll_tick = part != nullptr ? static_cast<uint32_t>(std::max(part->start, 0))
                                              : static_cast<uint32_t>(quarters * sequence_->timebase);
    const std::vector<song::Note>& notes = sequence_->tracks[static_cast<std::size_t>(index)].notes;
    if (part != nullptr && !part->notes.empty())
    {
        const auto [lowest, highest] = std::minmax_element(part->notes.begin(), part->notes.end(), by_key);
        key_editor_.scroll_key = (lowest->key + highest->key) / 2;
    }
    else if (!notes.empty())
    {
        const auto [lowest, highest] = std::minmax_element(notes.begin(), notes.end(), by_note_key);
        key_editor_.scroll_key = (lowest->key + highest->key) / 2;
    }
    else
    {
        key_editor_.scroll_key = kMiddleC;
    }
    key_selection_.reset();
    UpdateKeyTrack();
}

void App::CloseKeyEditor()
{
    key_editor_open_ = false;
    key_editor_.selected.clear();
    key_editor_.gesture = KeyGesture();
    key_selection_.reset();
}

void App::UpdateKeyTrack()
{
    // The selection follows the version of the shown sequence that the editor shows.
    const std::optional<uint32_t> sequence = ShownSequence();
    format::Shared<ntdlib::commands::Sequence> shown;
    if (sequence && archive_version_ && *sequence < archive_version_->sequences.size())
    {
        shown = archive_version_->sequences[*sequence];
    }
    key_editor_.selected = FollowedSelection({key_sequence_, key_editor_track_, std::move(key_editor_.selected)},
                                             std::exchange(key_selection_, std::nullopt), shown);
    key_editor_.selected_controls =
        FollowedSelection({key_sequence_, key_editor_track_, std::move(key_editor_.selected_controls)},
                          std::exchange(control_selection_, std::nullopt), shown);
    key_sequence_ = shown;

    key_track_.notes.clear();
    key_track_.controls.clear();
    key_editor_.limits.clear();
    const song::Track* track = TrackNumbered(key_editor_track_);
    if (track == nullptr || !song_)
    {
        return;
    }

    // The editor shows the track's name, colour and notes from the arrangement. Each command's limits come from the
    // stretch that ran its first note.
    const auto index = static_cast<std::size_t>(track - sequence_->tracks.data());
    key_track_.name = song_->tracks[index].name;
    key_track_.colour = song_->tracks[index].colour;
    key_track_.ticks_per_quarter = sequence_->timebase;
    key_track_.song_length = sequence_->length;
    std::vector<edit::NotePlace> places;
    std::set<uint32_t> commands;
    for (const song::Note& note : track->notes)
    {
        key_track_.notes.push_back(
            {note.start, note.length, note.key, note.velocity, note.command, note.bank_slot, note.program});
        if (commands.insert(note.command).second)
        {
            places.push_back({note.command, track->stretches[note.stretch]});
        }
    }

    // A sequence that Hokuto can't edit leaves every note fixed, and its controller lanes empty. The edit of a note
    // there shows why. The limits of a lane's command come from the stretch that ran it first.
    if (sequence && archive_version_)
    {
        try
        {
            key_track_.console = edit::ConsoleOf(*archive_version_);
            key_track_.controls = ControlsOf(*archive_version_, *sequence, *sequence_, *track);
            for (const KeyControl& control : key_track_.controls)
            {
                if (commands.insert(control.offset).second)
                {
                    places.push_back({control.offset, track->stretches.at(control.stretch)});
                }
            }
            const std::vector<edit::NoteLimits> limits =
                edit::LimitsOf(*archive_version_, *sequence, places,
                               sequence_->loop ? std::optional<uint32_t>(sequence_->loop->end) : std::nullopt);
            for (std::size_t i = 0; i < places.size(); i++)
            {
                key_editor_.limits.emplace(places[i].command, limits[i]);
            }
        }
        catch (const std::exception&)
        {
            key_editor_.limits.clear();
            key_track_.controls.clear();
        }
    }
}

KeySelection App::SelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited,
                                 const std::set<uint32_t>& kept) const
{
    KeySelection selection{edited.file.sequences.at(sequence), key_editor_track_, {}};
    std::vector<uint32_t> commands;
    if (key_sequence_.SharesWith(file.sequences.at(sequence)))
    {
        commands.assign(kept.begin(), kept.end());
    }
    if (commands.empty() && edited.added.empty())
    {
        return selection;
    }

    // A command that the edit can't follow leaves nothing selected.
    try
    {
        const std::vector<uint32_t> after = edit::CommandsAfter(file, sequence, edited, commands);
        selection.commands.insert(after.begin(), after.end());
    }
    catch (const std::invalid_argument&)
    {
        selection.commands.clear();
    }

    return selection;
}

KeySelection App::ListSelectionAfter(const format::File& file, uint32_t sequence, const edit::Edited& edited) const
{
    KeySelection selection{edited.file.sequences.at(sequence), list_editor_track_, {}};
    if (list_editor_.selected.empty() || !list_sequence_.SharesWith(file.sequences.at(sequence)))
    {
        return selection;
    }

    // Each selected command's item leads to its item in the new version.
    const std::vector<uint32_t> before = edit::Offsets(*file.sequences[sequence], edit::ConsoleOf(file));
    const std::vector<uint32_t> after = edit::Offsets(*edited.file.sequences[sequence], edit::ConsoleOf(file));
    for (const uint32_t offset : list_editor_.selected)
    {
        const std::optional<uint32_t> item = edit::ItemAtOffset(before, offset);
        const std::optional<uint32_t> moved = item && *item < edited.items.size() ? edited.items[*item] : std::nullopt;
        if (moved && *moved + 1 < after.size())
        {
            selection.commands.insert(after[*moved]);
        }
    }

    return selection;
}

void App::UpdateKeyEditor()
{
    if (!key_editor_open_ || !sequence_ || FrontOfLowerZone() != LowerZone::kKeyEditor)
    {
        key_editor_.request = KeyRequest::kNone;
        key_editor_.focused = false;
        PlayFeedback(std::nullopt);
        return;
    }

    // While the reader builds the archive of another version, the notes wait for it.
    const bool busy = reading_version_ != nullptr;
    const std::optional<KeyEdit> edit = DrawKeyEditor(key_track_, key_editor_, transport_, busy, &key_editor_open_);
    if (!key_editor_open_)
    {
        CloseKeyEditor();
    }
    else if (edit)
    {
        ApplyKeyEdit(*edit);
    }
    PlayFeedback(key_editor_open_ ? key_editor_.sounding : std::nullopt);
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
    if (engine_sound_ && engine_sound_->wii && engine_sound_->sound < engine_sound_->wii->Sounds().size())
    {
        // On the Wii's engine, the note plays the conversion's one bank. The notes of every slot play that bank there.
        // A slot whose bank went into that bank plays the programs of its range.
        const std::shared_ptr<const ntdlib::rsar::SoundArchive>& archive = engine_sound_->wii;
        const auto range = engine_sound_->slot_programs.find(note->bank_slot);
        const uint32_t program =
            note->program + (range != engine_sound_->slot_programs.end() ? range->second : uint32_t{0});
        PlayAudition(std::make_unique<song::NotePreview>(
            archive, std::vector<uint32_t>{archive->Sounds()[engine_sound_->sound].bank}, 0, program, note->key,
            note->velocity, release, engine_sound_->wii_build));
    }
    else if (engine_sound_ && engine_sound_->archive && engine_sound_->sound < engine_sound_->archive->Sounds().size())
    {
        // On another console's engine, the note plays with the banks of the sound's conversion.
        const std::shared_ptr<const ntdlib::sar::SoundArchive>& archive = engine_sound_->archive;
        PlayAudition(std::make_unique<song::NotePreview>(archive, archive->Sounds()[engine_sound_->sound].banks,
                                                         note->bank_slot, note->program, note->key, note->velocity,
                                                         release, model_mixing_));
    }
    else if (wii_archive_ && sequence_->sound < wii_archive_->Sounds().size())
    {
        PlayAudition(std::make_unique<song::NotePreview>(
            wii_archive_, std::vector<uint32_t>{wii_archive_->Sounds()[sequence_->sound].bank}, note->bank_slot,
            note->program, note->key, note->velocity, release, sequence_->wii_build));
    }
    else if (archive_ && sequence_->sound < archive_->Sounds().size())
    {
        PlayAudition(std::make_unique<song::NotePreview>(archive_, archive_->Sounds()[sequence_->sound].banks,
                                                         note->bank_slot, note->program, note->key, note->velocity,
                                                         release, model_mixing_));
    }
    else
    {
        return;
    }

    feedback_note_ = note;
    feedback_release_ = std::move(release);
}

void App::ApplyKeyEdit(const KeyEdit& edit)
{
    if (!edit.refused.empty())
    {
        ShowMessage(edit.refused);
        return;
    }
    if (!edit.changed_controls.empty() || !edit.moved_controls.empty() || !edit.added_controls.empty() ||
        !edit.removed_controls.empty())
    {
        ApplyControlEdit(edit);
        return;
    }

    // The editor's notes lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    const song::Track* track = TrackNumbered(key_editor_track_);
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        track == nullptr)
    {
        return;
    }

    // Each command's edit runs in the stretch that ran its first note.
    std::map<uint32_t, song::Stretch> stretches;
    for (const song::Note& note : track->notes)
    {
        stretches.emplace(note.command, track->stretches[note.stretch]);
    }
    std::vector<edit::NoteEdit> edits;
    for (const KeyChange& change : edit.changes)
    {
        const auto stretch = stretches.find(change.command);
        if (stretch != stretches.end())
        {
            edits.push_back({change.command, stretch->second, change.shift, change.transpose, change.velocity,
                             change.length, change.remove});
        }
    }
    if (edits.empty() && edit.added.empty())
    {
        return;
    }

    // The edited notes stay selected. New notes become the selection.
    try
    {
        std::vector<edit::NewNote> additions;
        for (const KeyNote& added : edit.added)
        {
            additions.push_back(NewNoteOf(*document, *sequence, *track, key_track_.controls, added));
        }
        edit::Edited edited = edit::EditNotes(*document, *sequence, edits, additions);
        KeySelection after = SelectionAfter(*document, *sequence, edited,
                                            edit.added.empty() ? key_editor_.selected : std::set<uint32_t>());
        list_selection_ = ListSelectionAfter(*document, *sequence, edited);

        // The Undo History names the track and the notes.
        const std::size_t notes = edits.size() + additions.size();
        Do(edit.name, "Track: " + key_track_.name + " - " + (notes == 1 ? "Note" : std::to_string(notes) + " Notes"),
           std::move(edited.file), std::move(after));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyControlEdit(const KeyEdit& edit)
{
    // The lanes' points lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    const song::Track* track = TrackNumbered(key_editor_track_);
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        track == nullptr)
    {
        return;
    }

    // Each command's edit runs in the stretch that ran it first. A new command goes into the last stretch that starts
    // by its tick (edit::StretchAt). A new note goes there too.
    std::map<uint32_t, song::Stretch> stretches;
    for (const KeyControl& control : key_track_.controls)
    {
        stretches.emplace(control.offset, track->stretches.at(control.stretch));
    }

    const auto place = [&stretches](uint32_t offset)
    {
        return edit::CommandPlace{offset, stretches.at(offset)};
    };
    try
    {
        edit::Edited edited;
        std::vector<uint32_t> kept;
        std::size_t count = 0;
        if (!edit.removed_controls.empty())
        {
            std::vector<edit::CommandPlace> places;
            for (const uint32_t offset : edit.removed_controls)
            {
                places.push_back(place(offset));
            }
            edited = edit::RemoveCommands(*document, *sequence, places);
            count = places.size();
        }
        else if (!edit.added_controls.empty())
        {
            std::vector<edit::NewSetting> settings;
            for (const auto& [tick, command] : edit.added_controls)
            {
                const std::optional<song::Stretch> stretch = edit::StretchAt(track->stretches, tick);
                if (!stretch)
                {
                    throw std::invalid_argument("A new command can't go at that tick on this track.");
                }

                settings.push_back({*stretch, tick, command});
            }
            edited = edit::AddSettings(*document, *sequence, settings);
            count = settings.size();
        }
        else
        {
            // The commands move first, and then take their new values.
            std::vector<edit::SettingMove> moves;
            for (const auto& [offset, shift] : edit.moved_controls)
            {
                moves.push_back({offset, stretches.at(offset), shift});
                kept.push_back(offset);
            }
            std::vector<edit::CommandPlace> places;
            std::vector<ntdlib::commands::Command> commands;
            for (const auto& [offset, command] : edit.changed_controls)
            {
                places.push_back(place(offset));
                commands.push_back(command);
                if (!edit.moved_controls.contains(offset))
                {
                    kept.push_back(offset);
                }
            }
            edited = moves.empty() ? edit::ChangeCommands(*document, *sequence, places, commands)
                                   : edit::MoveAndChangeCommands(*document, *sequence, moves, places, commands);
            count = kept.size();
        }

        // The changed commands and the new ones stay selected in the new version.
        const ntdlib::commands::Sequence& commands = *edited.file.sequences.at(*sequence);
        const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(edited.file));
        KeySelection after{edited.file.sequences[*sequence], key_editor_track_, {}};
        for (const uint32_t offset : kept)
        {
            const std::optional<uint32_t> item = edited.items.at(edit::ItemOf(*document, *sequence, offset));
            if (item && *item < commands.items.size())
            {
                after.commands.insert(offsets[*item]);
            }
        }
        for (const uint32_t item : edited.added)
        {
            after.commands.insert(offsets.at(item));
        }
        control_selection_ = std::move(after);

        // The Undo History names the track and the setting.
        std::string what = std::to_string(count) + " Commands";

        const auto setting_name = [](const ntdlib::commands::Command& command)
        {
            return LaneName(SettingOf(command).value_or(LaneSetting{command.kind}));
        };
        if (count == 1 && !edit.added_controls.empty())
        {
            what = setting_name(edit.added_controls.front().second);
        }
        else if (count == 1 && !edit.changed_controls.empty())
        {
            what = setting_name(edit.changed_controls.begin()->second);
        }
        else if (count == 1 && !edit.moved_controls.empty())
        {
            what = setting_name(edit::CommandAt(*document, *sequence, edit.moved_controls.begin()->first));
        }
        else if (count == 1)
        {
            what = setting_name(edit::CommandAt(*document, *sequence, *edit.removed_controls.begin()));
        }

        // The selected notes and the List Editor's selected commands stay selected.
        KeySelection notes = SelectionAfter(*document, *sequence, edited, key_editor_.selected);
        list_selection_ = ListSelectionAfter(*document, *sequence, edited);
        Do(edit.name, "Track: " + key_track_.name + " - " + what, std::move(edited.file), std::move(notes));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

std::pair<int, std::optional<int32_t>> App::ListEditorChoice() const
{
    // The selected part's track, or else the selected track, or else the first track.
    const PartView* part = SelectedPart(song_, arrangement_);
    const auto tracks = static_cast<int>(sequence_->tracks.size());
    std::optional<int32_t> start;
    int number = sequence_->tracks.empty() ? 0 : sequence_->tracks.front().number;
    if (part != nullptr && arrangement_.selected_part_track >= 0 && arrangement_.selected_part_track < tracks)
    {
        number = sequence_->tracks[static_cast<std::size_t>(arrangement_.selected_part_track)].number;
        start = part->start;
    }
    else if (arrangement_.selected_track >= 0 && arrangement_.selected_track < tracks)
    {
        number = sequence_->tracks[static_cast<std::size_t>(arrangement_.selected_track)].number;
    }

    return {number, start};
}

void App::OpenListEditor()
{
    if (!sequence_)
    {
        return;
    }

    const auto [number, start] = ListEditorChoice();
    list_editor_open_ = true;
    lower_front_ = LowerZone::kListEditor;
    list_editor_track_ = number;
    list_editor_.selected.clear();
    list_editor_.selected_label.reset();
    list_editor_.anchor.reset();
    list_editor_.typing.reset();
    list_selection_.reset();
    list_subroutine_.reset();
    UpdateListTrack();

    // The editor selects the row where the part starts, and scrolls to it: a call's row, or the part's first note's. A
    // row after a wait of unknown length has no start.
    list_editor_.scroll_row = 0;
    const PartView* part = SelectedPart(song_, arrangement_);
    const auto played = static_cast<std::size_t>(arrangement_.selected_part_track);
    if (start && part != nullptr && played < sequence_->tracks.size())
    {
        if (const std::optional<std::size_t> row =
                RowOfPart(list_track_, sequence_->tracks[played], part->first, *start))
        {
            list_editor_.selected = {list_track_.rows[*row].row.command};
            list_editor_.anchor = *row;
            list_editor_.scroll_row = *row;
        }
    }
}

void App::OpenMarker(const MarkerPlace& place)
{
    if (!sequence_ || !song_ || place.marker >= song_->logic.size() || song_->logic[place.marker].commands.empty())
    {
        return;
    }

    // The place's commands on the track of its first command.
    const LogicMarkerView& marker = song_->logic[place.marker];
    const int track = marker.commands.front().track;
    std::vector<uint32_t> commands;
    for (const MarkerCommand& command : marker.commands)
    {
        if (command.track == track)
        {
            commands.push_back(command.offset);
        }
    }
    OpenCommands(track, commands, place.tick);
}

void App::OpenCommands(int track, std::span<const uint32_t> commands, int32_t tick)
{
    list_editor_open_ = true;
    lower_front_ = LowerZone::kListEditor;
    list_editor_track_ = track;
    list_editor_.selected.clear();
    list_editor_.selected_label.reset();
    list_editor_.anchor.reset();
    list_editor_.typing.reset();
    list_selection_.reset();
    list_subroutine_.reset();
    UpdateListTrack();

    // The commands, and the first row that runs one of them at the tick.
    list_editor_.selected.insert(commands.begin(), commands.end());
    list_editor_.scroll_row = 0;
    for (std::size_t i = 0; i < list_track_.rows.size(); i++)
    {
        const ListRow& row = list_track_.rows[i];
        if (!IsLabel(row) && row.row.tick == static_cast<uint32_t>(tick) &&
            list_editor_.selected.contains(row.row.command))
        {
            list_editor_.scroll_row = i;
            break;
        }
    }
}

void App::CloseListEditor()
{
    list_editor_open_ = false;
    list_editor_.selected.clear();
    list_editor_.selected_label.reset();
    list_editor_.anchor.reset();
    list_editor_.typing.reset();
    list_selection_.reset();
    list_subroutine_.reset();
}

void App::UpdateListTrack()
{
    // The selection follows the version of the shown sequence that the editor shows.
    const std::optional<uint32_t> sequence = ShownSequence();
    format::Shared<ntdlib::commands::Sequence> shown;
    if (sequence && archive_version_ && *sequence < archive_version_->sequences.size())
    {
        shown = archive_version_->sequences[*sequence];
    }
    list_editor_.selected = FollowedSelection({list_sequence_, list_editor_track_, std::move(list_editor_.selected)},
                                              std::exchange(list_selection_, std::nullopt), shown);
    list_sequence_ = shown;
    if (!list_editor_open_ || !sequence_ || !sequence || !archive_version_)
    {
        list_track_ = ListTrack{};
        return;
    }

    // The editor shows the track's name and colour from the arrangement. A subroutine that it shows by itself shows the
    // rows of one run. Each label has a row before its command.
    FollowSubroutine(shown);
    list_track_ = MakeListTrack(*archive_version_, *sequence, *sequence_, list_editor_track_);
    for (std::size_t i = 0; song_ && i < sequence_->tracks.size() && i < song_->tracks.size(); i++)
    {
        if (sequence_->tracks[i].number == list_editor_track_)
        {
            list_track_.name = song_->tracks[i].name;
            list_track_.colour = song_->tracks[i].colour;
        }
    }
    if (list_subroutine_)
    {
        list_track_ = WithSubroutine(std::move(list_track_), *list_subroutine_);
    }
    list_track_ = WithLabelRows(std::move(list_track_));

    // A command that the player of the engine that plays the sound skips shows grey.
    if (engine_sound_)
    {
        list_track_.engine = engine_sound_->wii ? song::Console{.wii = engine_sound_->wii_build}
                                                : song::Console{.flavor = engine_sound_->archive->ReadOptions().flavor};
    }
    list_track_.engine_name = EngineName(PlayedEngine());
    const format::File* document = Document();
    list_track_.file_engine_name = EngineName(document != nullptr ? document->engine : PlayedEngine());
}

void App::FollowSubroutine(const format::Shared<ntdlib::commands::Sequence>& shown)
{
    if (!list_subroutine_ || !shown || !sequence_ || !archive_version_)
    {
        list_subroutine_.reset();
        return;
    }

    // In another version, the subroutine's label finds its first command.
    const std::vector<uint32_t> offsets = edit::Offsets(*shown, edit::ConsoleOf(*archive_version_));
    if (!list_subroutine_version_.SharesWith(shown))
    {
        const auto& labels = shown->labels;
        const auto label = std::find_if(labels.begin(), labels.end(),
                                        [this](const auto& other) { return other.name == list_subroutine_->name; });
        if (list_subroutine_->name.empty() || label == labels.end() || label->item >= offsets.size())
        {
            list_subroutine_.reset();
            return;
        }

        list_subroutine_->first = offsets[label->item];
        list_subroutine_version_ = shown;
    }

    // The runs in the shown sound, and the calls in the sequence. The shown run keeps its place among the runs.
    ListSubroutine& subroutine = *list_subroutine_;
    subroutine.runs = SubroutineRuns(*sequence_, subroutine.first);
    subroutine.sound_calls = CallsOfRuns(*sequence_, subroutine.runs);
    subroutine.loops = sequence_->loop.has_value();
    const std::optional<uint32_t> item = edit::ItemAtOffset(offsets, subroutine.first);
    if (subroutine.runs.empty() || !item)
    {
        list_subroutine_.reset();
        return;
    }

    subroutine.shown = std::min(subroutine.shown, subroutine.runs.size() - 1);
    subroutine.calls = static_cast<int>(edit::CallsTo(*shown, *item).size());
    list_editor_track_ = subroutine.runs[subroutine.shown].track;
}

void App::OpenSubroutine(const ListEditorState::SubroutineChoice& choice)
{
    const std::optional<uint32_t> sequence = ShownSequence();
    if (!sequence || !sequence_ || !archive_version_ || *sequence >= archive_version_->sequences.size())
    {
        return;
    }

    // The subroutine takes the name of the first label at its first command, in the order of the file.
    const format::Shared<ntdlib::commands::Sequence>& shown = archive_version_->sequences[*sequence];
    const std::vector<uint32_t> offsets = edit::Offsets(*shown, edit::ConsoleOf(*archive_version_));
    const std::optional<uint32_t> item = edit::ItemAtOffset(offsets, choice.first);
    ListSubroutine subroutine;
    subroutine.first = choice.first;
    for (const auto& label : shown->labels)
    {
        if (item && label.item == *item)
        {
            subroutine.name = label.name;
            break;
        }
    }
    subroutine.runs = SubroutineRuns(*sequence_, choice.first);
    subroutine.sound_calls = CallsOfRuns(*sequence_, subroutine.runs);
    subroutine.loops = sequence_->loop.has_value();
    if (subroutine.runs.empty())
    {
        ShowMessage("No call of the shown sound runs the subroutine.", MessageKind::kNote);
        return;
    }

    // The chosen run, or the first on the shown track, or the first. std::find of a ListCall fails to compile under
    // clang-cl with Microsoft's STL 14.51 (microsoft/STL#6294).
    const auto& runs = subroutine.runs;
    auto run = choice.run ? std::find_if(runs.begin(), runs.end(),
                                         [&choice](const ListCall& other) { return other == *choice.run; })
                          : runs.end();
    if (run == runs.end())
    {
        run = std::find_if(runs.begin(), runs.end(),
                           [this](const ListCall& other) { return other.track == list_editor_track_; });
    }
    subroutine.shown = run == runs.end() ? 0 : static_cast<std::size_t>(run - runs.begin());

    list_subroutine_ = std::move(subroutine);
    list_subroutine_version_ = shown;
    list_editor_.selected.clear();
    list_editor_.selected_label.reset();
    list_editor_.anchor.reset();
    list_editor_.typing.reset();
    list_selection_.reset();
    UpdateListTrack();
    list_editor_.scroll_row = 0;
}

void App::CloseSubroutine()
{
    if (!list_subroutine_)
    {
        return;
    }

    const std::optional<ListCall> run = list_subroutine_->shown < list_subroutine_->runs.size()
                                            ? std::optional<ListCall>(list_subroutine_->runs[list_subroutine_->shown])
                                            : std::nullopt;
    list_subroutine_.reset();
    list_editor_.selected.clear();
    list_editor_.selected_label.reset();
    list_editor_.anchor.reset();
    list_editor_.typing.reset();
    list_selection_.reset();
    if (run)
    {
        list_editor_track_ = run->track;
    }
    UpdateListTrack();

    // The call that started the run ends the stretch before it.
    list_editor_.scroll_row = 0;
    for (std::size_t i = list_track_.rows.size(); run && i-- > 0;)
    {
        const ListRow& row = list_track_.rows[i];
        if (!IsLabel(row) && row.row.stretch + 1 == run->stretch && row.command.kind == ntdlib::commands::Kind::kCall)
        {
            list_editor_.selected = {row.row.command};
            list_editor_.anchor = i;
            list_editor_.scroll_row = i;
            break;
        }
    }
}

void App::UpdateListEditor()
{
    if (!list_editor_open_ || !sequence_ || FrontOfLowerZone() != LowerZone::kListEditor)
    {
        list_editor_.focused = false;
        return;
    }

    // While the reader builds the archive of another version, the edits wait for it.
    const bool busy = reading_version_ != nullptr;
    const std::optional<ListEdit> edit =
        DrawListEditor(list_track_, list_editor_, transport_, busy, &list_editor_open_);
    if (!list_editor_open_)
    {
        CloseListEditor();
        return;
    }

    // The edit goes first: it names the rows that the editor showed. Another track then shows from its first row. A
    // subroutine shows by itself, or its track shows again.
    if (edit)
    {
        ApplyListEdit(*edit);
    }
    if (const std::optional<int> chosen = std::exchange(list_editor_.chosen_track, std::nullopt))
    {
        list_editor_track_ = *chosen;
        list_editor_.selected.clear();
        list_editor_.selected_label.reset();
        list_editor_.anchor.reset();
        list_editor_.typing.reset();
        list_selection_.reset();
        list_subroutine_.reset();
        UpdateListTrack();
        list_editor_.scroll_row = 0;
    }
    if (const auto choice = std::exchange(list_editor_.open_subroutine, std::nullopt))
    {
        OpenSubroutine(*choice);
    }
    if (std::exchange(list_editor_.close_subroutine, false))
    {
        CloseSubroutine();
    }
}

void App::ApplyListEdit(const ListEdit& edit)
{
    if (!edit.refused.empty())
    {
        ShowMessage(edit.refused);
        return;
    }

    // The editor's rows lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        edit.rows.empty() || edit.rows.front() >= list_track_.rows.size())
    {
        return;
    }

    try
    {
        // A change, a move and a new command go through the row's place. A deletion takes each row's command.
        const std::size_t first = edit.rows.front();
        const edit::CommandPlace place = PlaceOf(list_track_, first);
        edit::Edited edited;
        switch (edit.action)
        {
        case ListEdit::Action::kChange:
            edited = edit::ChangeCommand(*document, *sequence, place, edit.command);
            break;

        case ListEdit::Action::kMove:
            if (list_track_.rows[first].command.kind == ntdlib::commands::Kind::kNote)
            {
                const edit::NoteEdit note{.command = place.command, .stretch = place.stretch, .shift = edit.shift};
                edited = edit::EditNotes(*document, *sequence, std::span(&note, 1));
            }
            else
            {
                const edit::SettingMove move{.command = place.command, .stretch = place.stretch, .shift = edit.shift};
                edited = edit::MoveSettings(*document, *sequence, std::span(&move, 1));
            }
            break;

        case ListEdit::Action::kInsert:
            edited = edit::InsertCommand(*document, *sequence, place, list_track_.rows[first].entered, edit.command);
            break;

        case ListEdit::Action::kRemove:
            {
                std::vector<edit::CommandPlace> places;
                for (const std::size_t row : edit.rows)
                {
                    places.push_back(PlaceOf(list_track_, row));
                }
                edited = edit::RemoveCommands(*document, *sequence, places);
                break;
            }

        case ListEdit::Action::kAddLabel:
            edited = edit::AddLabel(*document, *sequence, place.command, edit.label);
            break;

        case ListEdit::Action::kRenameLabel:
            edited = edit::RenameLabel(*document, *sequence, edit.label, edit.new_name);
            break;

        case ListEdit::Action::kMoveLabel:
            edited = edit::MoveLabel(*document, *sequence, edit.label, place.command);
            break;

        case ListEdit::Action::kRemoveLabel:
            edited = edit::RemoveLabel(*document, *sequence, edit.label);
            break;

        case ListEdit::Action::kToSubroutine:
            {
                std::vector<uint32_t> selected;
                for (const std::size_t row : edit.rows)
                {
                    selected.push_back(list_track_.rows.at(row).row.command);
                }
                edited = edit::ToSubroutine(*document, *sequence, selected, edit.depth, edit.label);
                break;
            }

        case ListEdit::Action::kDissolveCall:
            edited = edit::DissolveCall(*document, *sequence, place.command);
            break;

        case ListEdit::Action::kCopySubroutine:
            edited = edit::CopySubroutine(*document, *sequence, place.command);
            break;
        }

        // The changed, moved or new command stays selected in the new version, and so does a new call or a new copy of
        // a subroutine's commands. A label that an edit adds, renames or moves stays selected. The Key Editor's notes
        // stay selected too.
        using Action = ListEdit::Action;
        const ntdlib::commands::Sequence& commands = *edited.file.sequences.at(*sequence);
        const std::vector<uint32_t> offsets = edit::Offsets(commands, edit::ConsoleOf(edited.file));
        std::vector<std::optional<uint32_t>> items;
        std::optional<std::string> label;
        switch (edit.action)
        {
        case Action::kInsert:
        case Action::kToSubroutine:
            if (!edited.added.empty())
            {
                items.emplace_back(edited.added.front());
            }
            break;

        case Action::kDissolveCall:
            items.assign(edited.added.begin(), edited.added.end());
            break;

        case Action::kAddLabel:
        case Action::kMoveLabel:
            label = edit.label;
            break;

        case Action::kRenameLabel:
            label = edit.new_name;
            break;

        case Action::kRemove:
        case Action::kRemoveLabel:
            break;

        default:
            items.push_back(edited.items.at(edit::ItemOf(*document, *sequence, place.command)));
            break;
        }

        KeySelection after{edited.file.sequences[*sequence], list_editor_track_, {}};
        for (const std::optional<uint32_t>& item : items)
        {
            if (item && *item < commands.items.size())
            {
                after.commands.insert(offsets[*item]);
            }
        }
        list_selection_ = std::move(after);
        list_editor_.selected_label = label;
        KeySelection notes = SelectionAfter(*document, *sequence, edited, key_editor_.selected);

        // A renamed subroutine keeps showing by itself. One without a label follows its first command.
        if (list_subroutine_ && edit.action == Action::kRenameLabel && edit.label == list_subroutine_->name)
        {
            list_subroutine_->name = edit.new_name;
        }
        else if (list_subroutine_ && list_subroutine_->name.empty())
        {
            const std::optional<uint32_t> moved =
                edited.items.at(edit::ItemOf(*document, *sequence, list_subroutine_->first));
            if (moved && *moved < commands.items.size())
            {
                list_subroutine_->first = offsets[*moved];
                list_subroutine_version_ = edited.file.sequences[*sequence];
            }
        }

        // The Undo History names the track and the command, or the label or subroutine.
        const bool inserted = edit.action == Action::kInsert;
        std::string what =
            edit.rows.size() == 1
                ? std::string(KindName(inserted ? edit.command.kind : list_track_.rows[first].command.kind))
                : std::to_string(edit.rows.size()) + " Commands";
        switch (edit.action)
        {
        case Action::kAddLabel:
        case Action::kMoveLabel:
        case Action::kRemoveLabel:
        case Action::kToSubroutine:
            what = edit.label;
            break;

        case Action::kRenameLabel:
            what = edit.label + " to " + edit.new_name;
            break;

        case Action::kDissolveCall:
        case Action::kCopySubroutine:
            what = CellText(list_track_, list_track_.rows[first], ListColumn::kData1);
            break;

        default:
            break;
        }

        Do(edit.name, "Track: " + list_track_.name + " - " + what, std::move(edited.file), std::move(notes));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyTempoMove(const TempoMove& move)
{
    // The point leads to the command of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    const song::Track* track = TrackNumbered(move.point.track);
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        track == nullptr || move.point.stretch >= track->stretches.size())
    {
        return;
    }

    // A tempo that the console's bytes round to the command's tempo changes nothing.
    try
    {
        const std::optional<ntdlib::commands::Command> command =
            move.point.bpm ? TempoCommand(*document, *sequence, move.point, move.to) : std::nullopt;
        const ntdlib::commands::Command& old = edit::CommandAt(*document, *sequence, move.point.command);
        const bool changed = command && command->value.value != old.value.value;
        const edit::CommandPlace place{move.point.command, track->stretches[move.point.stretch]};
        if (move.shift == 0 && !changed)
        {
            return;
        }

        // The Undo History names every drag of a tempo point Edit Tempo Track. The details name the track and the
        // tempos. The details of an FX channel's volume take the same form. The editors' selections stay.
        if (move.shift == 0)
        {
            const std::string details = "Track: " + move.point.track_name + " - from " +
                                        ValueText(old.kind, old.value.value) + " to " +
                                        ValueText(command->kind, command->value.value);
            edit::Edited edited = edit::ChangeCommand(*document, *sequence, place, *command);
            KeySelection notes = SelectionAfter(*document, *sequence, edited, key_editor_.selected);
            list_selection_ = ListSelectionAfter(*document, *sequence, edited);
            Do("Edit Tempo Track", details, std::move(edited.file), std::move(notes));
            return;
        }

        // A move goes first, and the new tempo after it.
        const edit::SettingMove setting{place.command, place.stretch, move.shift};
        edit::Edited edited = changed ? edit::MoveAndChangeCommands(*document, *sequence, std::span(&setting, 1),
                                                                    std::span(&place, 1), std::span(&*command, 1))
                                      : edit::MoveSettings(*document, *sequence, std::span(&setting, 1));
        KeySelection notes = SelectionAfter(*document, *sequence, edited, key_editor_.selected);
        list_selection_ = ListSelectionAfter(*document, *sequence, edited);
        Do("Edit Tempo Track", "Track: " + move.point.track_name + " - Tempo", std::move(edited.file),
           std::move(notes));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyPartEdit(const PartEdit& part_edit)
{
    if (!part_edit.refused.empty())
    {
        ShowMessage(part_edit.refused);
        return;
    }

    // The part's notes lead to the commands of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        !sequence_ || !song_ || part_edit.track < 0 ||
        static_cast<std::size_t>(part_edit.track) >= sequence_->tracks.size() ||
        static_cast<std::size_t>(part_edit.track) >= song_->tracks.size())
    {
        return;
    }

    const song::Track& track = sequence_->tracks[static_cast<std::size_t>(part_edit.track)];
    const TrackView& view = song_->tracks[static_cast<std::size_t>(part_edit.track)];
    if (part_edit.part < 0 || static_cast<std::size_t>(part_edit.part) >= view.parts.size())
    {
        return;
    }

    // Each command of the part's notes takes the edit once, in the stretch that ran its first note there.
    const PartView& part = view.parts[static_cast<std::size_t>(part_edit.part)];
    std::vector<edit::NoteEdit> edits;
    std::set<uint32_t> commands;
    for (std::size_t n = part.first; n < part.first + part.notes.size() && n < track.notes.size(); n++)
    {
        const song::Note& note = track.notes[n];
        if (!commands.insert(note.command).second)
        {
            continue;
        }

        edit::NoteEdit& note_edit =
            edits.emplace_back(edit::NoteEdit{.command = note.command, .stretch = track.stretches[note.stretch]});
        switch (part_edit.value)
        {
        case PartValue::kStart:
            note_edit.shift = part_edit.amount;
            break;
        case PartValue::kTranspose:
            note_edit.transpose = part_edit.amount;
            break;
        case PartValue::kVelocity:
            note_edit.velocity = std::clamp(note.velocity + part_edit.amount, 1, 127);
            break;
        }
    }
    if (edits.empty())
    {
        return;
    }

    // The Undo History names a part's new start Move, a transposition Transpose and a velocity Change Part Velocity.
    // The part's notes stay selected in the Key Editor.
    try
    {
        edit::Edited edited = edit::EditNotes(*document, *sequence, edits, {});
        KeySelection after = SelectionAfter(*document, *sequence, edited, key_editor_.selected);
        list_selection_ = ListSelectionAfter(*document, *sequence, edited);
        const char* name = part_edit.value == PartValue::kStart       ? "Move"
                           : part_edit.value == PartValue::kTranspose ? "Transpose"
                                                                      : "Change Part Velocity";
        const std::string details = "Track: " + view.name + " - " +
                                    (part.notes.size() == 1 ? "Note" : std::to_string(part.notes.size()) + " Notes");
        Do(name, details, std::move(edited.file), std::move(after));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyLoopMove(const LoopMove& move)
{
    // The loop is the loop of the version that built the open archive.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        move.start < 0 || move.end <= move.start)
    {
        return;
    }

    // The Undo History names a drag of a loop's edge Size. The details name the sound and the edge.
    try
    {
        const edit::ConvertedLoop loop{static_cast<uint32_t>(move.start), static_cast<uint32_t>(move.end)};
        edit::Edited moved = edit::MoveLoop(*document, *sequence, loop);
        KeySelection notes = SelectionAfter(*document, *sequence, moved, key_editor_.selected);
        list_selection_ = ListSelectionAfter(*document, *sequence, moved);
        const double quarter = std::max(song_ ? song_->ticks_per_quarter : 48, 1);
        const int beats = song_ ? song_->beats_per_bar : 4;
        const auto position = [quarter, beats](int32_t tick)
        {
            return WithoutFigureSpaces(PositionText(tick / quarter, beats));
        };
        const std::string details =
            "Sound: " + (song_ ? song_->name : std::string()) + " - " +
            (move.end_edge ? "Loop End from " + position(move.from_end) + " to " + position(move.end)
                           : "Loop Start from " + position(move.from_start) + " to " + position(move.start));
        Do("Size", details, std::move(moved.file), std::move(notes));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyMixMoves()
{
    for (std::size_t i = 0; song_ && i < song_->tracks.size(); i++)
    {
        TrackView& view = song_->tracks[i];
        const std::optional<MixMove> move = std::exchange(view.moved, std::nullopt);
        if (move && !(sequence_ && i < sequence_->tracks.size() && ApplyMixMove(sequence_->tracks[i], *move)))
        {
            view.held.reset();
        }
    }
}

bool App::ApplyMixMove(const song::Track& track, const MixMove& move)
{
    // The mixer's tracks lead to the commands of the version that built the open archive. A control holds its new value
    // until the new version's sequence shows it, or until a move in the history changes the archive (FollowHistory).
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    const int from = MixValue(move.control, move.from);
    const int to = MixValue(move.control, move.to);
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        from == to)
    {
        return false;
    }

    try
    {
        edit::Edited edited =
            edit::EditMix(*document, *sequence, track.number, track.stretches, {MixOf(move.control), from, to});
        KeySelection after = SelectionAfter(*document, *sequence, edited, key_editor_.selected);
        list_selection_ = ListSelectionAfter(*document, *sequence, edited);
        const ActionNames names = MixMoveNames(track.name, move.control, from, to);
        Do(names.name, names.details, std::move(edited.file), std::move(after));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
        return false;
    }

    return true;
}

void App::ApplyInstrumentChoices()
{
    for (std::size_t i = 0; song_ && i < song_->tracks.size(); i++)
    {
        const std::optional<InstrumentChoice> choice = std::exchange(song_->tracks[i].instrument_choice, std::nullopt);
        if (choice && sequence_ && i < sequence_->tracks.size())
        {
            ApplyInstrumentChoice(sequence_->tracks[i], *choice);
        }
    }
}

void App::ApplyInstrumentChoice(const song::Track& track, const InstrumentChoice& choice)
{
    // The track's instrument is the one that its first note plays. The edit goes into the version that built the open
    // archive. The mixer's edits do the same.
    const format::File* document = Document();
    const std::optional<uint32_t> sequence = ShownSequence();
    if (document == nullptr || !archive_version_ || !sequence || !SameArchive(*document, *archive_version_) ||
        track.notes.empty())
    {
        return;
    }

    const edit::Program from{track.notes.front().bank_slot, static_cast<int>(track.notes.front().program)};
    const edit::Program to{choice.slot, choice.program};
    if (from == to)
    {
        return;
    }

    try
    {
        edit::Edited edited = edit::EditInstrument(*document, *sequence, track.number, track.stretches, from, to);
        KeySelection after = SelectionAfter(*document, *sequence, edited, key_editor_.selected);
        list_selection_ = ListSelectionAfter(*document, *sequence, edited);
        const std::string details =
            "Track: " + track.name + " - " + InstrumentText(*song_, from) + " to " + InstrumentText(*song_, to);
        Do(to.slot != from.slot ? "Change Bank" : "Change Program", details, std::move(edited.file), std::move(after));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::ApplyVariableChoices()
{
    if (!song_)
    {
        return;
    }

    // A variable has its initial value without a choice: -1, or 0 for a global variable on the Wii. A variable holds 16
    // bits.
    constexpr int kLowest = std::numeric_limits<int16_t>::min();
    constexpr int kHighest = std::numeric_limits<int16_t>::max();
    const std::optional<uint32_t> sound = ShownSound();
    bool chosen = false;
    for (VariableView& variable : song_->variables)
    {
        const std::optional<int> value = std::exchange(variable.chosen, std::nullopt);
        if (!value || !sound)
        {
            continue;
        }

        song::StartValues& values = start_values_[*sound];
        if (*value == variable.initial)
        {
            values.erase(variable.number);
        }
        else
        {
            values[variable.number] = static_cast<int16_t>(std::clamp(*value, kLowest, kHighest));
        }
        chosen = true;
    }
    if (chosen && sequence_)
    {
        ReadShown();
    }
}

void App::UpdateScenes()
{
    if (!song_)
    {
        return;
    }

    song_->scenes.clear();
    scene_places_.clear();
    const std::optional<uint32_t> sound = ShownSound();
    const format::File* document = Document();
    if (!sound || document == nullptr)
    {
        return;
    }

    const song::StartValues values = StartValuesOf(*sound);
    const song::Changes changes = ChangesOf(*sound);
    for (std::size_t place = 0; place < document->scenes.size(); place++)
    {
        const format::Scene& scene = document->scenes[place];
        if (scene.sound == *sound)
        {
            song_->scenes.push_back({scene.name, SceneStartValues(scene) == values && SceneChanges(scene) == changes});
            scene_places_.push_back(place);
        }
    }
}

void App::ApplySceneRequest()
{
    const std::optional<SceneRequest> request =
        song_ ? std::exchange(song_->scene_request, std::nullopt) : std::nullopt;
    const std::optional<uint32_t> sound = ShownSound();
    const format::File* document = Document();
    if (!request || !sound || document == nullptr)
    {
        return;
    }

    // Store makes a scene of the values that the variables start with, and of their changes. It takes the first free
    // name.
    format::File file = *document;
    const std::string where = "Sound: " + song_->name + " - ";
    if (request->action == SceneAction::kStore)
    {
        const std::string name = NewSceneName(file.scenes, *sound);
        file.scenes.push_back({*sound, name, SceneValues(StartValuesOf(*sound)), FileChanges(ChangesOf(*sound))});
        Do("Store Scene", where + name, std::move(file));
        return;
    }

    // The other actions take a scene of the sound. A scene that the file no longer holds takes none.
    if (request->scene >= scene_places_.size() || scene_places_[request->scene] >= file.scenes.size())
    {
        return;
    }

    const std::size_t place = scene_places_[request->scene];
    format::Scene& scene = file.scenes[place];
    const std::string name = scene.name;
    switch (request->action)
    {
    case SceneAction::kRecall:
        RecallScene(*sound, scene);
        break;

    case SceneAction::kUpdate:
        scene.values = SceneValues(StartValuesOf(*sound));
        scene.changes = FileChanges(ChangesOf(*sound));
        if (scene != document->scenes[place])
        {
            Do("Update Scene", where + name, std::move(file));
        }
        break;

    case SceneAction::kRename:
        scene.name = request->name;
        Do("Rename Scene", where + "from " + name + " to " + request->name, std::move(file));
        break;

    case SceneAction::kDelete:
        file.scenes.erase(file.scenes.begin() + static_cast<std::ptrdiff_t>(place));
        Do("Delete Scene", where + name, std::move(file));
        break;

    case SceneAction::kStore:
        break;
    }
}

void App::ShowTrackColours()
{
    arrangement_.custom_colours = settings_.track_colours == TrackColours::kCustom;
    if (!song_)
    {
        return;
    }

    // A track that the file gives no colour keeps its rainbow colour while the colours are Custom.
    const format::File* document = Document();
    for (std::size_t i = 0; i < song_->tracks.size(); i++)
    {
        TrackView& track = song_->tracks[i];
        track.colour =
            settings_.track_colours == TrackColours::kStandard ? theme::kStandardTrackColour : track.rainbow_colour;
        if (settings_.track_colours != TrackColours::kCustom || document == nullptr || !sequence_ ||
            i >= sequence_->tracks.size())
        {
            continue;
        }

        const format::TrackPlace place{sequence_->sound, static_cast<uint8_t>(sequence_->tracks[i].number)};
        if (const auto found = document->track_colours.find(place); found != document->track_colours.end())
        {
            track.colour = IM_COL32(found->second.red, found->second.green, found->second.blue, 255);
        }
    }

    // The Key Editor shows its track's colour.
    if (sequence_)
    {
        if (const song::Track* track = TrackNumbered(key_editor_track_))
        {
            key_track_.colour = song_->tracks[static_cast<std::size_t>(track - sequence_->tracks.data())].colour;
        }
    }
}

void App::ApplyColourPick()
{
    const std::optional<TrackColourPick> pick = std::exchange(arrangement_.colour_pick, std::nullopt);
    const format::File* document = Document();
    if (!pick || document == nullptr || !sequence_ || !song_ || pick->track < 0 ||
        static_cast<std::size_t>(pick->track) >= sequence_->tracks.size())
    {
        return;
    }

    // The file keeps the colour by the sound and the track's number. Automatic drops it.
    const auto index = static_cast<std::size_t>(pick->track);
    const format::TrackPlace place{sequence_->sound, static_cast<uint8_t>(sequence_->tracks[index].number)};
    format::File file = *document;
    if (pick->colour)
    {
        const ImU32 colour = *pick->colour;
        file.track_colours[place] = {static_cast<uint8_t>(colour >> IM_COL32_R_SHIFT),
                                     static_cast<uint8_t>(colour >> IM_COL32_G_SHIFT),
                                     static_cast<uint8_t>(colour >> IM_COL32_B_SHIFT)};
    }
    else
    {
        file.track_colours.erase(place);
    }
    if (file.track_colours != document->track_colours)
    {
        Do("Change Track Colour", "Sound: " + song_->name + " - " + song_->tracks[index].name, std::move(file));
    }
}

void App::ApplyTagChange(SoundSection& section)
{
    const std::optional<TagChange> change = std::exchange(section.tag_changed, std::nullopt);
    const format::File* document = Document();
    if (!change || document == nullptr || !section.view)
    {
        return;
    }

    Do("Change Tag", "Sound: " + section.view->values.name + " - " + TagLabel(change->key),
       format::WithTag(*document, section.view->sound, change->key, change->value));
}

void App::ApplySoundChange(SoundSection& section)
{
    const std::optional<edit::SoundChange> change = std::exchange(section.changed, std::nullopt);
    const format::File* document = Document();
    if (!change || document == nullptr || !section.view ||
        edit::ValueOf(section.view->values, change->value, change->slot) == change->to)
    {
        return;
    }

    try
    {
        format::File file = edit::ChangeSound(*document, section.view->sound, *change);
        const ActionNames names = SoundChangeNames(*section.view, *change);
        Do(names.name, names.details, std::move(file));
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::RecallScene(uint32_t sound, const format::Scene& scene)
{
    start_values_[sound] = SceneStartValues(scene);
    changes_[sound] = SceneChanges(scene);
    ShowChanges(sound);
    if (sequence_)
    {
        ReadShown();
        return;
    }

    // The made-up song shows the values and nothing more.
    const song::StartValues& values = start_values_[sound];
    for (VariableView& variable : song_->variables)
    {
        const auto found = values.find(variable.number);
        variable.start = found != values.end() ? found->second : -1;
    }
}

std::optional<uint32_t> App::BrowsedSound() const
{
    // A wave sound file's wave sound shows too.
    const BrowserPlace& place = browser_state_.selected;
    const bool sound =
        place.group == static_cast<int>(kSoundsGroup) || place.group == static_cast<int>(kSequencesGroup);
    const bool listed = place.group == static_cast<int>(kWaveSoundFilesGroup) && place.child >= 0 && place.item >= 0;
    if (listed)
    {
        const std::vector<std::vector<uint32_t>>& items = browser_children_[kWaveSoundFilesGroup];
        const auto item = static_cast<std::size_t>(place.item);
        const auto child = static_cast<std::size_t>(place.child);
        return item < items.size() && child < items[item].size() ? std::optional<uint32_t>(items[item][child])
                                                                 : std::nullopt;
    }
    if (!sound || place.child >= 0 || place.item < 0)
    {
        return std::nullopt;
    }

    const std::vector<uint32_t>& items = browser_items_[static_cast<std::size_t>(place.group)];
    const auto item = static_cast<std::size_t>(place.item);
    return item < items.size() ? std::optional<uint32_t>(items[item]) : std::nullopt;
}

std::optional<uint32_t> App::ShownSound() const
{
    if (sequence_)
    {
        return sequence_->sound;
    }

    return preview_ && song_ ? std::optional<uint32_t>(0) : std::nullopt;
}

void App::UpdateVariables()
{
    if (!song_ || !sequence_)
    {
        return;
    }

    // Each variable keeps the last value that the clock gave until the clock gives another. A stop clears them.
    const std::optional<song::PlaybackClock::Variables> values =
        clock_ ? clock_->VariablesAt(HeardSamples()) : std::nullopt;
    for (VariableView& variable : song_->variables)
    {
        if (!clock_)
        {
            variable.value.reset();
        }
        else if (values && variable.number >= 0 && variable.number < song::kGameVariables)
        {
            variable.value = (*values)[static_cast<std::size_t>(variable.number)];
        }
    }
}

song::StartValues App::StartValuesOf(uint32_t sound) const
{
    const auto found = start_values_.find(sound);
    return found != start_values_.end() ? found->second : song::StartValues();
}

song::Changes App::ChangesOf(uint32_t sound) const
{
    const auto found = changes_.find(sound);
    return found != changes_.end() ? found->second : song::Changes();
}

void App::ApplyChangeRequest()
{
    const std::optional<ChangeRequest> request =
        song_ ? std::exchange(song_->change_request, std::nullopt) : std::nullopt;
    const std::optional<uint32_t> sound = ShownSound();
    if (!request || !sound)
    {
        return;
    }
    if (!request->refused.empty())
    {
        ShowMessage(request->refused);
        return;
    }

    // A variable holds 16 bits. A change's time counts milliseconds in 32 bits.
    constexpr int kLowest = std::numeric_limits<int16_t>::min();
    constexpr int kHighest = std::numeric_limits<int16_t>::max();
    song::Changes& changes = changes_[*sound];
    switch (request->action)
    {
    case ChangeAction::kAdd:
        {
            // A new change comes at the position. It sets the first variable that the zone lists, or else the first
            // that the song reads, to the choice after the value that the variable starts with, or to the next value.
            const std::vector<VariableView>& offered =
                song_->variables.empty() ? song_->read_variables : song_->variables;
            if (offered.empty())
            {
                break;
            }

            const VariableView& variable = offered.front();
            const song::StartValues starts = StartValuesOf(*sound);
            const auto found = starts.find(variable.number);
            const int start = found != starts.end() ? found->second : variable.initial;
            const auto chosen = std::find(variable.choices.begin(), variable.choices.end(), start);
            int value = start < kHighest ? start + 1 : start - 1;
            if (!variable.choices.empty())
            {
                value = chosen == variable.choices.end() || std::next(chosen) == variable.choices.end()
                            ? variable.choices.front()
                            : *std::next(chosen);
            }

            // The change takes the time that the display shows.
            const uint64_t time = std::min<uint64_t>(WholeMs(PositionTime()), std::numeric_limits<uint32_t>::max());
            changes.push_back({static_cast<uint32_t>(time), variable.number,
                               static_cast<int16_t>(std::clamp(value, kLowest, kHighest))});
            break;
        }

    case ChangeAction::kEdit:
        if (request->change < changes.size())
        {
            const ChangeView& edited = request->edited;
            changes[request->change] = {edited.time, edited.variable,
                                        static_cast<int16_t>(std::clamp(edited.value, kLowest, kHighest))};
        }
        break;

    case ChangeAction::kRemove:
        if (request->change < changes.size())
        {
            changes.erase(changes.begin() + static_cast<std::ptrdiff_t>(request->change));
        }
        break;
    }

    // The changes keep the order of their times. The sequence plays them once it's read again.
    std::ranges::stable_sort(changes, {}, &song::VariableChange::time);
    ShowChanges(*sound);
    if (sequence_)
    {
        ReadShown();
    }
}

void App::ShowChanges(uint32_t sound)
{
    if (!song_)
    {
        return;
    }

    // Without a sequence, the made-up song keeps one tempo.
    song_->changes.clear();
    for (const song::VariableChange& change : ChangesOf(sound))
    {
        const double tick = sequence_ ? song::TickOfMs(*sequence_, change.time)
                                      : change.time * song_->tempo * song_->ticks_per_quarter / 60'000.0;
        song_->changes.push_back({change.time, change.variable, change.value, tick});
    }
}

double App::PositionTime() const
{
    if (sequence_)
    {
        return song::MsOfTick(*sequence_, transport_.Position() * sequence_->timebase);
    }

    return transport_.Position() * 60'000.0 / transport_.Tempo();
}

std::optional<uint32_t> App::ShownSequence() const
{
    return sequence_ ? SequenceOfSound(sequence_->sound) : std::nullopt;
}

std::optional<uint32_t> App::SequenceOfSound(uint32_t sound) const
{
    std::optional<uint32_t> file;
    if (archive_ && sound < archive_->Sounds().size())
    {
        file = archive_->Sounds()[sound].file_id;
    }
    else if (wii_archive_ && sound < wii_archive_->Sounds().size())
    {
        file = wii_archive_->Sounds()[sound].file_id;
    }

    return file && *file < file_sequences_.size() ? file_sequences_[*file] : std::nullopt;
}

std::vector<std::optional<edit::SoundPlace>> App::Places() const
{
    if (!start_fields_)
    {
        return {};
    }

    return wii_archive_ ? edit::SoundPlaces(*wii_archive_, file_sequences_, *start_fields_)
           : archive_   ? edit::SoundPlaces(*archive_, file_sequences_, *start_fields_)
                        : std::vector<std::optional<edit::SoundPlace>>();
}

std::vector<std::string> App::SoundNames() const
{
    std::vector<std::string> names;
    if (wii_archive_)
    {
        for (const ntdlib::rsar::SoundInfo& sound : wii_archive_->Sounds())
        {
            names.push_back(sound.name);
        }
    }
    else if (archive_)
    {
        for (const ntdlib::sar::SoundInfo& sound : archive_->Sounds())
        {
            names.push_back(sound.name);
        }
    }

    return names;
}

const song::Track* App::TrackNumbered(int number) const
{
    if (!sequence_)
    {
        return nullptr;
    }

    const auto found = std::find_if(sequence_->tracks.begin(), sequence_->tracks.end(),
                                    [number](const song::Track& track) { return track.number == number; });
    return found != sequence_->tracks.end() ? &*found : nullptr;
}

const song::Track* App::SelectedTrack() const
{
    const int index = arrangement_.selected_track;
    if (!sequence_ || index < 0 || static_cast<std::size_t>(index) >= sequence_->tracks.size())
    {
        return nullptr;
    }

    return &sequence_->tracks[static_cast<std::size_t>(index)];
}

bool App::WindowWaits() const
{
    return replace_.has_value() || export_.has_value() || new_project_open_;
}

bool App::TracksEditable() const
{
    const format::File* document = Document();
    return history_ && !preview_ && sequence_ && (archive_ || wii_archive_) && archive_version_ && start_fields_ &&
           ShownSequence().has_value() && document != nullptr && document->native && document->native->tables &&
           SameArchive(*document, *archive_version_) && reading_version_ == nullptr && !WindowWaits();
}

void App::EditTracks(TrackAction action)
{
    if (!TracksEditable())
    {
        return;
    }

    // The edit finds each sound's start in the version that built the open archive.
    const format::File& document = *Document();
    const std::vector<std::optional<edit::SoundPlace>> places = Places();
    const uint32_t sound = sequence_->sound;
    const song::Track* selected = SelectedTrack();
    try
    {
        switch (action)
        {
        case TrackAction::kAdd:
            {
                // The new track plays the instrument of the selected track's first note.
                std::optional<edit::Program> instrument;
                if (selected != nullptr && !selected->notes.empty())
                {
                    const song::Note& first = selected->notes.front();
                    instrument = edit::Program{first.bank_slot, static_cast<int>(first.program)};
                }
                edit::TrackEdit added = edit::AddTrack(document, places, sound, *sequence_, instrument);
                track_to_select_ = added.track;
                Do("Add Track", "Sound: " + song_->name + " - Track " + std::to_string(added.track),
                   std::move(added.file));
                break;
            }

        case TrackAction::kDuplicate:
            {
                if (selected == nullptr)
                {
                    return;
                }

                edit::TrackEdit copied = edit::DuplicateTrack(document, places, sound, selected->number);
                track_to_select_ = copied.track;
                Do("Duplicate Track",
                   "Sound: " + song_->name + " - Track " + std::to_string(selected->number) + " to Track " +
                       std::to_string(copied.track),
                   std::move(copied.file));
                break;
            }

        case TrackAction::kRemove:
            {
                if (selected == nullptr)
                {
                    return;
                }

                const int number = selected->number;
                format::File removed = edit::RemoveTrack(document, places, sound, number);
                Do("Remove Track", "Sound: " + song_->name + " - Track " + std::to_string(number), std::move(removed));
                break;
            }

        case TrackAction::kCopy:
            {
                if (selected == nullptr)
                {
                    return;
                }

                copied_track_ = edit::CopyTrack(document, places, sound, *sequence_, selected->number);
                break;
            }

        case TrackAction::kPaste:
            {
                if (!copied_track_)
                {
                    return;
                }

                // The details name the sound that the track came from.
                const std::vector<format::NativeSound>& sounds = document.native->tables->sounds;
                const std::string from =
                    copied_track_->sound < sounds.size() ? sounds[copied_track_->sound].name : std::string();
                edit::TrackEdit pasted = edit::PasteTrack(document, places, sound, *sequence_, *copied_track_);
                track_to_select_ = pasted.track;
                Do("Paste Track",
                   "Sound: " + song_->name + " - Track " + std::to_string(copied_track_->track) + " of " + from +
                       " to Track " + std::to_string(pasted.track),
                   std::move(pasted.file));
                break;
            }
        }
    }
    catch (const std::exception& error)
    {
        ShowMessage(error.what());
    }
}

void App::UpdatePlayback()
{
    // Stop while stopped ends a preview: the browser's, a window's Play, an Export clip or the test tone.
    if (transport_.IdleStops() != idle_stops_)
    {
        idle_stops_ = transport_.IdleStops();
        StopPreview();
    }

    if (!sequence_)
    {
        transport_.Advance(std::min(ImGui::GetIO().DeltaTime, kLongestStep));
        return;
    }

    // A new model that has caught up with playback takes over at the player's next frame. One that has run while
    // playback was stopped plays the next playback.
    // A model of another engine takes over by a new playback, from about the sample that the device plays.
    if (catch_up_.Finished())
    {
        model_ = catch_up_.Take();
        if (const std::optional<format::Engine> engine = std::exchange(engine_catch_up_, std::nullopt))
        {
            model_engine_ = *engine;
            if (clock_ && transport_.Playing())
            {
                StartPlayback(ChangeOverTick());
            }
        }
        else if (clock_)
        {
            clock_->Offer(model_, catch_up_.ByTicks());
        }
    }

    // A drag in the ruler moves the position once the mouse is released.
    const bool scrubbing = arrangement_.ruler.MovingPosition();
    if (!transport_.Playing())
    {
        StopPlayback();
    }
    else if (!scrubbing && (!clock_ || transport_.Moves() != playback_moves_))
    {
        StartPlayback();
    }
    if (!clock_)
    {
        return;
    }

    clock_->SetCycle(CycleInTicks());
    clock_->SetMutes(MutedTracks());
    clock_->SetBusVolumes(BusVolumes());
    if (!scrubbing)
    {
        if (const std::optional<double> ticks = clock_->PositionAt(HeardSamples()))
        {
            transport_.Follow(*ticks / sequence_->timebase);
        }
    }

    // Playback stops at the end of the sequence, once the device has taken the last frame.
    if (clock_->Ended() && ring_.Available() == 0)
    {
        transport_.Stop();
        StopPlayback();
    }
}

void App::StartPlayback(std::optional<uint32_t> start)
{
    // A new model that's still running stops where it has got to, and this playback plays it.
    if (catch_up_.Pending())
    {
        model_ = catch_up_.Take();
        if (const std::optional<format::Engine> engine = std::exchange(engine_catch_up_, std::nullopt))
        {
            model_engine_ = *engine;
        }
    }

    // Playback starts from the position, even past the end of the sequence, or from `start`.
    const double kMost = std::numeric_limits<uint32_t>::max();
    const double ticks = start ? *start : std::clamp(transport_.Position() * sequence_->timebase, 0.0, kMost);
    clock_ = std::make_shared<song::PlaybackClock>(model_->FrameSamples());
    clock_->SetCycle(CycleInTicks());
    clock_->SetMutes(MutedTracks());
    clock_->SetBusVolumes(BusVolumes());
    playback_moves_ = transport_.Moves();
    metered_ = 0;
    browser_state_.playing.reset();
    wave_state_.playing = false;
    replace_previewing_ = false;
    auto player = std::make_unique<song::SequencePlayer>(model_, static_cast<uint32_t>(ticks), clock_);
    playback_rate_ = player->SampleRate();
    if (audition_rate_ != playback_rate_)
    {
        audition_worker_.Stop();
    }
    output_.SetSourceRate(playback_rate_);
    output_.Resume();
    worker_.Play(std::move(player));
}

uint32_t App::ChangeOverTick() const
{
    // The position that the device plays, and the model's position in the arrangement. A looped sequence's model counts
    // its ticks on past the loop.
    const double kMost = std::numeric_limits<uint32_t>::max();
    const double heard = std::clamp(transport_.Position() * sequence_->timebase, 0.0, kMost);
    const double after = std::clamp(model_->After(), 0.0, kMost);
    const song::Sequence& played = model_->GetSequence();
    const double at = played.loop ? song::PositionOfTicks(played, static_cast<uint64_t>(after)) : after;

    // Within a quarter of a second, playback goes on from the model's next tick. The model then runs on without a seek.
    // Further away, such as after the cycle went back, playback starts at the position.
    const double frames = 0.25 * model_->SampleRate() / static_cast<double>(model_->FrameSamples());
    const double near = frames * model_->TicksPerFrame(after);
    return static_cast<uint32_t>(std::abs(at - heard) <= near ? std::ceil(after) : heard);
}

uint64_t App::HeardSamples() const
{
    // The device plays the sample that comes before those in the ring and in the device's buffer.
    const uint64_t written = clock_->Written();
    const uint64_t queued = QueuedSamples();
    return written > queued ? written - queued : 0;
}

uint64_t App::QueuedSamples() const
{
    // The ring's frames are at the player's rate.
    uint64_t queued = ring_.Available();
    if (output_.DeviceRate() > 0)
    {
        queued += static_cast<uint64_t>(output_.DeviceBufferFrames()) * static_cast<uint64_t>(playback_rate_) /
                  static_cast<uint64_t>(output_.DeviceRate());
    }

    return queued;
}

song::PlaybackClock::CycleRange App::CycleInTicks() const
{
    // The transport counts quarter notes. A locator before the start counts as the start.
    const double quarter = sequence_->timebase;
    const auto ticks = [quarter](double quarters)
    {
        const double kMost = std::numeric_limits<uint32_t>::max();
        return static_cast<uint32_t>(std::clamp(std::round(quarters * quarter), 0.0, kMost));
    };
    return {transport_.Cycle(), ticks(transport_.CycleStart()), ticks(transport_.CycleEnd())};
}

uint32_t App::MutedTracks() const
{
    uint32_t tracks = 0;
    if (!song_)
    {
        return tracks;
    }

    for (std::size_t i = 0; i < song_->tracks.size() && i < sequence_->tracks.size(); i++)
    {
        if (!IsHeard(*song_, i))
        {
            tracks |= 1u << sequence_->tracks[i].number;
        }
    }

    return tracks;
}

song::AuxVolumes App::BusVolumes() const
{
    const format::File* document = Document();
    if (document == nullptr)
    {
        return song::kFullVolumes;
    }

    song::AuxVolumes volumes = document->aux_volumes;
    if (fx_.held)
    {
        volumes[fx_.held->channel] = GainOfDecibels(fx_.volumes[fx_.held->channel]);
    }

    return volumes;
}

void App::FollowBusVolumes()
{
    const format::File* document = Document();
    for (std::size_t channel = 0; document != nullptr && channel < kFxChannels; channel++)
    {
        if (!fx_.held || fx_.held->channel != channel)
        {
            fx_.volumes[channel] = DecibelsOfGain(document->aux_volumes[channel]);
        }
    }
}

void App::ApplyFxMove()
{
    const std::optional<FxMove> move = std::exchange(fx_.moved, std::nullopt);
    const format::File* document = Document();
    if (!move || document == nullptr)
    {
        return;
    }

    // A move that ends where it started changes nothing.
    format::File file = *document;
    file.aux_volumes[move->channel] = GainOfDecibels(move->to);
    if (file.aux_volumes == document->aux_volumes)
    {
        return;
    }

    // The Undo History names the edit after the channel: FX A / Volume.
    const std::string channel = std::string("FX ") + static_cast<char>('A' + move->channel);
    Do(channel + " / Volume", "from " + LevelText(move->from) + " dB to " + LevelText(move->to) + " dB",
       std::move(file));
}

void App::StopPlayback()
{
    if (clock_)
    {
        worker_.Stop();
        clock_.reset();
    }
}

void App::Present()
{
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu_);
    if (commands == nullptr)
    {
        return;
    }

    // The swapchain has no texture while the window is hidden. The command buffer goes to the GPU anyway.
    SDL_GPUTexture* swapchain = nullptr;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(commands, window_, &swapchain, nullptr, nullptr) &&
        swapchain != nullptr && !minimized)
    {
        // The vertex and index buffers go up before the render pass starts.
        ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, commands);

        SDL_GPUColorTargetInfo target{};
        target.texture = swapchain;
        target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(draw_data, commands, pass);
        SDL_EndGPURenderPass(pass);
    }
    SDL_SubmitGPUCommandBuffer(commands);
}

} // namespace hokuto::app
