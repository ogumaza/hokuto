// SPDX-License-Identifier: MIT

// The chunks of the binary form other than the sequences, the native archive, the banks, the waves and the streams:
// the tables, the build, the sizes, the mixer, scenes, track colours, instrument and bank maps, the editor, tags,
// lengths, timelines and the engine.

#include "format/file_parts.h"

namespace hokuto::format
{
namespace
{

using file_parts::Damaged;
using file_parts::In;
using file_parts::Newer;
using file_parts::Out;

} // namespace

namespace file_parts
{

// TABL: the native archive's tables. The sounds come first, then the names of the banks and the wave archives, the
// players, and the names of the groups. Each list starts with its length.
std::vector<uint8_t> TablesChunk(const NativeTables& tables)
{
    Out out;
    out.Count(tables.sounds.size(), "sounds");
    for (const NativeSound& sound : tables.sounds)
    {
        out.Name(sound.name, "bytes in a sound name");
        out.U8(CodeOf(sound.kind));
        out.U8(sound.volume);
        out.U8(sound.pan_mode);
        out.U8(sound.pan_curve);
        out.U8(sound.player_priority);
        out.U8(sound.actor_player);
        out.U8(sound.channel_priority);
        out.U8(sound.release_priority_fix ? kReleasePriorityFix : 0);
        out.U32(sound.player);
        out.U32(sound.tracks);
        out.Count(sound.banks.size(), "banks of a sound");
        for (const uint32_t bank : sound.banks)
        {
            out.U32(bank);
        }
    }

    for (const std::vector<std::string>* names : {&tables.banks, &tables.wave_archives})
    {
        out.Count(names->size(), "items in the tables");
        for (const std::string& name : *names)
        {
            out.Name(name, "bytes in an item's name");
        }
    }

    out.Count(tables.players.size(), "players");
    for (const NativePlayer& player : tables.players)
    {
        out.Name(player.name, "bytes in a player name");
        out.U32(player.sound_limit);
    }

    out.Count(tables.groups.size(), "groups");
    for (const std::string& name : tables.groups)
    {
        out.Name(name, "bytes in a group name");
    }

    return out.Take();
}

NativeTables ReadTables(std::span<const uint8_t> chunk)
{
    In in(chunk, "the tables");
    NativeTables tables;
    const uint32_t sounds = in.U32();
    for (uint32_t i = 0; i < sounds; ++i)
    {
        NativeSound& sound = tables.sounds.emplace_back();
        sound.name = in.Name();
        const uint8_t code = in.U8();
        const std::optional<ntdlib::sar::SoundType> kind = SoundTypeOfCode(code);
        if (!kind)
        {
            Newer("a sound type (" + std::to_string(code) + ")");
        }

        sound.kind = *kind;
        sound.volume = in.U8();
        sound.pan_mode = in.U8();
        sound.pan_curve = in.U8();
        sound.player_priority = in.U8();
        sound.actor_player = in.U8();
        sound.channel_priority = in.U8();
        const uint8_t flags = in.U8();
        if ((flags & ~kReleasePriorityFix) != 0)
        {
            Newer("a sound flag");
        }

        sound.release_priority_fix = (flags & kReleasePriorityFix) != 0;
        sound.player = in.U32();
        sound.tracks = in.U32();
        const uint32_t banks = in.U32();
        for (uint32_t b = 0; b < banks; ++b)
        {
            sound.banks.push_back(in.U32());
        }
    }

    for (std::vector<std::string>* names : {&tables.banks, &tables.wave_archives})
    {
        const uint32_t count = in.U32();
        for (uint32_t i = 0; i < count; ++i)
        {
            names->push_back(in.Name());
        }
    }

    const uint32_t players = in.U32();
    for (uint32_t i = 0; i < players; ++i)
    {
        NativePlayer& player = tables.players.emplace_back();
        player.name = in.Name();
        player.sound_limit = in.U32();
    }

    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        tables.groups.push_back(in.Name());
    }

    return tables;
}

// BULD: the game's build of NW4R SND. The date of its banner, then the flags and 3 zeros.
std::vector<uint8_t> BuildChunk(const ntdlib::SndBuild& build)
{
    Out out;
    out.U32(build.date);
    out.U8(build.october_2006 ? kOctober2006 : 0);
    out.Zeros(3);

    return out.Take();
}

ntdlib::SndBuild ReadBuild(std::span<const uint8_t> chunk)
{
    In in(chunk, "the BULD chunk");
    ntdlib::SndBuild build;
    build.date = in.U32();
    const uint8_t flags = in.U8();
    if ((flags & ~kOctober2006) != 0)
    {
        Newer("a build of the Wii sound library");
    }

    build.october_2006 = (flags & kOctober2006) != 0;

    return build;
}

// SIZE: the game's sizes. The archive's size, then the number of groups and each group's size.
std::vector<uint8_t> SizesChunk(const GameSizes& sizes)
{
    Out out;
    out.U64(sizes.archive);
    out.Count(sizes.groups.size(), "groups' sizes");
    for (const uint64_t size : sizes.groups)
    {
        out.U64(size);
    }

    return out.Take();
}

GameSizes ReadSizes(std::span<const uint8_t> chunk)
{
    In in(chunk, "the SIZE chunk");
    GameSizes sizes;
    sizes.archive = in.U64();
    const uint32_t groups = in.U32();
    for (uint32_t i = 0; i < groups; ++i)
    {
        sizes.groups.push_back(in.U64());
    }

    return sizes;
}

// Whether an aux bus's volume differs from full volume.
bool NotUnity(float volume)
{
    return volume != 1.0f;
}

// MIXR: the effects on the aux buses, then their volumes. For each of the buses A to C, the number of its effects, then
// each effect: its kind, three zeros, the number of its parameters, and each parameter as a float. After bus C's
// effects, each bus's volume as a float, unless every bus is at full volume.
std::vector<uint8_t> MixerChunk(const File& file)
{
    Out out;
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        out.Count(file.aux_effects[bus].size(), "effects on a bus");
        for (const Effect& effect : file.aux_effects[bus])
        {
            if (const std::optional<std::string> problem = EffectProblem(effect))
            {
                throw std::invalid_argument("an effect on aux bus " + BusName(bus) + " " + *problem);
            }

            out.U8(CodeOf(effect.kind));
            out.Zeros(3);
            out.Count(effect.parameters.size(), "parameters of an effect");
            for (const float parameter : effect.parameters)
            {
                out.F32(parameter);
            }
        }
    }

    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        if (const std::optional<std::string> problem = AuxVolumeProblem(file.aux_volumes[bus]))
        {
            throw std::invalid_argument("aux bus " + BusName(bus) + "'s volume " + *problem);
        }
    }

    if (std::any_of(file.aux_volumes.begin(), file.aux_volumes.end(), NotUnity))
    {
        for (const float volume : file.aux_volumes)
        {
            out.F32(volume);
        }
    }

    return out.Take();
}

void ReadMixer(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the mixer");
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        const uint32_t count = in.U32();
        for (uint32_t e = 0; e < count; ++e)
        {
            const uint8_t code = in.U8();
            const std::optional<EffectKind> kind = EffectKindOfCode(code);
            if (!kind)
            {
                Newer("an effect (" + std::to_string(code) + ")");
            }

            Effect& effect = file.aux_effects[bus].emplace_back();
            effect.kind = *kind;
            in.Take(3);
            const uint32_t parameters = in.U32();
            for (uint32_t p = 0; p < parameters; ++p)
            {
                effect.parameters.push_back(in.F32());
            }

            if (const std::optional<std::string> problem = EffectProblem(effect))
            {
                Damaged("an effect on aux bus " + BusName(bus) + " " + *problem);
            }
        }
    }

    // A chunk that ends after bus C's effects leaves each bus at full volume.
    if (in.Left() == 0)
    {
        return;
    }

    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        file.aux_volumes[bus] = in.F32();
        if (const std::optional<std::string> problem = AuxVolumeProblem(file.aux_volumes[bus]))
        {
            Damaged("aux bus " + BusName(bus) + "'s volume " + *problem);
        }
    }
}

// SCEN: the scenes. Their number, then each scene: its sound, its name, the number of its starting values and each one,
// then the number of its changes and each one. A starting value is the variable's number, a zero and the value. A
// change is its time, then the same.
std::vector<uint8_t> ScenesChunk(const File& file)
{
    Out out;
    out.Count(file.scenes.size(), "scenes");
    for (std::size_t s = 0; s < file.scenes.size(); ++s)
    {
        const Scene& scene = file.scenes[s];
        if (const std::optional<std::string> problem = SceneProblem(scene))
        {
            throw std::invalid_argument("scene " + std::to_string(s) + " " + *problem);
        }

        out.U32(scene.sound);
        out.Name(scene.name, "a scene's name");
        out.Count(scene.values.size(), "starting values of a scene");
        for (const auto& [variable, value] : scene.values)
        {
            out.U8(variable);
            out.U8(0);
            out.U16(static_cast<uint16_t>(value));
        }

        out.Count(scene.changes.size(), "changes of a scene");
        for (const VariableChange& change : scene.changes)
        {
            out.U32(change.time);
            out.U8(change.variable);
            out.U8(0);
            out.U16(static_cast<uint16_t>(change.value));
        }
    }

    return out.Take();
}

} // namespace file_parts

namespace
{

// A variable's number and the zero after it. A later version may give the zero a meaning.
uint8_t ReadVariable(In& in)
{
    const uint8_t variable = in.U8();
    if (const uint8_t kind = in.U8(); kind != 0)
    {
        Newer("a variable type (" + std::to_string(kind) + ")");
    }

    return variable;
}

} // namespace

namespace file_parts
{

void ReadScenes(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the scenes");
    const uint32_t count = in.U32();
    for (uint32_t s = 0; s < count; ++s)
    {
        Scene& scene = file.scenes.emplace_back();
        scene.sound = in.U32();
        scene.name = in.Name();
        const uint32_t values = in.U32();
        for (uint32_t v = 0; v < values; ++v)
        {
            const uint8_t variable = ReadVariable(in);
            if (!scene.values.emplace(variable, static_cast<int16_t>(in.U16())).second)
            {
                Damaged("scene " + std::to_string(s) + " starts variable " + std::to_string(variable) + " twice");
            }
        }

        const uint32_t changes = in.U32();
        for (uint32_t c = 0; c < changes; ++c)
        {
            VariableChange& change = scene.changes.emplace_back();
            change.time = in.U32();
            change.variable = ReadVariable(in);
            change.value = static_cast<int16_t>(in.U16());
        }

        if (const std::optional<std::string> problem = SceneProblem(scene))
        {
            Damaged("scene " + std::to_string(s) + " " + *problem);
        }
    }
}

// TCOL: the tracks' colours. Their number, then each one: the sound, the track, and the colour's red, green and blue.
std::vector<uint8_t> TrackColoursChunk(const File& file)
{
    Out out;
    out.Count(file.track_colours.size(), "tracks' colours");
    for (const auto& [place, colour] : file.track_colours)
    {
        out.U32(place.sound);
        out.U8(place.track);
        out.U8(colour.red);
        out.U8(colour.green);
        out.U8(colour.blue);
    }

    return out.Take();
}

void ReadTrackColours(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the tracks' colours");
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        TrackPlace place;
        place.sound = in.U32();
        place.track = in.U8();
        Colour colour;
        colour.red = in.U8();
        colour.green = in.U8();
        colour.blue = in.U8();
        if (!file.track_colours.emplace(place, colour).second)
        {
            Damaged("it sets two colours for track " + std::to_string(place.track) + " of sound " +
                    std::to_string(place.sound));
        }
    }
}

// IMAP: the instrument maps. Their number, then each one: the voice group's address and the number of its pairings,
// then each pairing: a byte for the drum channel, the program, the voice and a byte of 0.
std::vector<uint8_t> InstrumentMapsChunk(const File& file)
{
    Out out;
    out.Count(file.instrument_maps.size(), "instrument maps");
    for (const auto& [voices, map] : file.instrument_maps)
    {
        out.U32(voices);
        out.Count(map.size(), "pairings of an instrument map");
        for (const auto& [instrument, voice] : map)
        {
            out.U8(instrument.drums ? 1 : 0);
            out.U8(static_cast<uint8_t>(instrument.program));
            out.U8(static_cast<uint8_t>(voice));
            out.U8(0);
        }
    }

    return out.Take();
}

void ReadInstrumentMaps(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the instrument maps");
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t voices = in.U32();
        const auto [map, added] = file.instrument_maps.try_emplace(voices);
        if (!added)
        {
            Damaged("it has two instrument maps for the voice group at address " + std::to_string(voices));
        }

        const uint32_t pairings = in.U32();
        for (uint32_t p = 0; p < pairings; ++p)
        {
            const uint8_t drums = in.U8();
            const uint8_t program = in.U8();
            const uint8_t voice = in.U8();
            in.U8();
            if (drums > 1)
            {
                Damaged("an instrument map holds a drum flag other than 0 or 1");
            }

            if (program > 127 || voice > 127)
            {
                Damaged("an instrument map pairs a program or a voice past 127");
            }

            if (!map->second.emplace(MidiInstrument{drums == 1, program}, voice).second)
            {
                Damaged("an instrument map pairs " + std::string(drums == 1 ? "drum program " : "program ") +
                        std::to_string(program) + " twice");
            }
        }
    }
}

// BMAP: the banks' instrument maps. Their number, then each one: the bank's index and the numbers of its pairings, then
// the pairing of each program and of each drum note.
std::vector<uint8_t> BankMapsChunk(const File& file)
{
    Out out;
    out.Count(file.bank_maps.size(), "banks' instrument maps");
    for (const auto& [bank, map] : file.bank_maps)
    {
        out.U32(bank);
        out.Count(map.programs.size(), "programs of an instrument map");
        out.Count(map.drum_notes.size(), "drum notes of an instrument map");
        for (const auto& [program, target] : map.programs)
        {
            out.U32(static_cast<uint32_t>(program));
            out.U32(static_cast<uint32_t>(target));
        }

        for (const auto& [note, target] : map.drum_notes)
        {
            out.U8(static_cast<uint8_t>(note));
            out.U8(static_cast<uint8_t>(target.key));
            out.U8(0);
            out.U8(0);
            out.U32(static_cast<uint32_t>(target.program));
        }
    }

    return out.Take();
}

void ReadBankMaps(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the banks' instrument maps");
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t bank = in.U32();
        const auto [map, added] = file.bank_maps.try_emplace(bank);
        if (!added)
        {
            Damaged("it has two instrument maps for bank " + std::to_string(bank));
        }

        const uint32_t programs = in.U32();
        const uint32_t notes = in.U32();
        for (uint32_t p = 0; p < programs; ++p)
        {
            const uint32_t program = in.U32();
            const uint32_t target = in.U32();
            if (program > 127 || target > kMostBankMapProgram)
            {
                Damaged("an instrument map pairs a MIDI program past 127 or a program past 65,535");
            }

            if (!map->second.programs.emplace(static_cast<int>(program), static_cast<int>(target)).second)
            {
                Damaged("an instrument map pairs program " + std::to_string(program) + " twice");
            }
        }

        for (uint32_t n = 0; n < notes; ++n)
        {
            const uint8_t note = in.U8();
            const uint8_t key = in.U8();
            in.U8();
            in.U8();
            const uint32_t target = in.U32();
            if (note > 127 || key > 127 || target > kMostBankMapProgram)
            {
                Damaged("an instrument map pairs a note or a key past 127, or a program past 65,535");
            }

            if (!map->second.drum_notes.emplace(note, MidiDrumNote{static_cast<int>(target), key}).second)
            {
                Damaged("an instrument map pairs drum note " + std::to_string(note) + " twice");
            }
        }
    }
}

// EDTR: the program's view. The sound, the position and the cycle's locators, then a byte for whether the cycle is on,
// and the numbers of the selected track and of the editors' tracks. The arrangement's sizes, scroll and sets of tracks,
// the Key Editor's sizes and scroll, then the number of lanes and each lane. A byte for the window in front ends the
// chunk.
std::vector<uint8_t> EditorChunk(const EditorView& view)
{
    if (const std::optional<std::string> problem = ViewProblem(view))
    {
        throw std::invalid_argument("the view " + *problem);
    }

    Out out;
    out.U32(view.sound);
    out.F32(view.position);
    out.F32(view.cycle_start);
    out.F32(view.cycle_end);
    out.U8(view.cycle ? 1 : 0);
    out.U8(view.selected_track.value_or(kNoTrack));
    out.U8(view.key_editor_track.value_or(kNoTrack));
    out.U8(view.list_editor_track.value_or(kNoTrack));
    out.F32(view.beat_width);
    out.F32(view.track_height);
    out.F32(view.left);
    out.F32(view.top);
    out.U32(view.muted);
    out.U32(view.soloed);
    out.F32(view.key_beat_width);
    out.F32(view.key_height);
    out.F32(view.key_left);
    out.F32(view.key_top);
    out.Count(view.lanes.size(), "lanes");
    for (const ViewLane& lane : view.lanes)
    {
        out.U16(CodeOf(lane.kind));
        out.U8(lane.index);
        out.U8(0);
    }

    out.U8(static_cast<uint8_t>(view.front));

    return out.Take();
}

// A chunk without the byte for the window in front puts the List Editor in front: Hokuto opened such a file with the
// List Editor in front where it opened. Bytes after that byte belong to later versions. A reader skips them.
void ReadEditor(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the view");

    const auto track = [](uint8_t number)
    {
        return number == kNoTrack ? std::nullopt : std::optional<uint8_t>(number);
    };

    EditorView view;
    view.sound = in.U32();
    view.position = in.F32();
    view.cycle_start = in.F32();
    view.cycle_end = in.F32();
    const uint8_t cycle = in.U8();
    view.selected_track = track(in.U8());
    view.key_editor_track = track(in.U8());
    view.list_editor_track = track(in.U8());
    view.beat_width = in.F32();
    view.track_height = in.F32();
    view.left = in.F32();
    view.top = in.F32();
    view.muted = in.U32();
    view.soloed = in.U32();
    view.key_beat_width = in.F32();
    view.key_height = in.F32();
    view.key_left = in.F32();
    view.key_top = in.F32();
    if (cycle > 1)
    {
        Damaged("the view's cycle is neither on nor off");
    }

    view.cycle = cycle == 1;
    const uint32_t lanes = in.U32();
    for (uint32_t i = 0; i < lanes; ++i)
    {
        const uint16_t code = in.U16();
        const std::optional<Kind> kind = KindOfCode(code);
        if (!kind)
        {
            Newer("a lane for a command (" + std::to_string(code) + ")");
        }

        view.lanes.push_back({*kind, in.U8()});
        in.U8();
    }

    view.front = ViewFront::kListEditor;
    if (in.Left() > 0)
    {
        const uint8_t front = in.U8();
        if (front > static_cast<uint8_t>(ViewFront::kListEditor))
        {
            Newer("a window in front of the editors (" + std::to_string(front) + ")");
        }

        view.front = static_cast<ViewFront>(front);
    }

    if (const std::optional<std::string> problem = ViewProblem(view))
    {
        Damaged("the view " + *problem);
    }

    file.view = std::move(view);
}

} // namespace file_parts

namespace
{

// The number of `tags`, then each tag: its key, then its value. `whose` names the tags in a message, such as "sound 3's
// ".
void WriteTags(Out& out, const Tags& tags, const std::string& whose)
{
    out.Count(tags.size(), "tags");
    for (const auto& [key, value] : tags)
    {
        if (const std::optional<std::string> problem = TagProblem(key, value))
        {
            throw std::invalid_argument(whose + *problem);
        }

        out.Name(key, "a tag's key");
        out.Name(value, "a tag's value");
    }
}

// Tags that WriteTags wrote.
Tags ReadTagSet(In& in, const std::string& whose)
{
    Tags tags;
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string key = in.Name();
        std::string value = in.Name();
        if (const std::optional<std::string> problem = TagProblem(key, value))
        {
            Damaged(whose + *problem);
        }

        if (tags.contains(key))
        {
            Damaged(whose + "tag " + key + " comes twice");
        }

        tags.emplace(std::move(key), std::move(value));
    }

    return tags;
}

} // namespace

namespace file_parts
{

// TAGS: the file's tags, then the number of sounds with tags, and each sound's index and tags.
std::vector<uint8_t> TagsChunk(const File& file)
{
    Out out;
    WriteTags(out, file.tags, "");
    const auto tagged = [](const auto& entry)
    {
        return !entry.second.empty();
    };
    out.Count(static_cast<std::size_t>(std::count_if(file.sound_tags.begin(), file.sound_tags.end(), tagged)),
              "sounds with tags");
    for (const auto& [sound, tags] : file.sound_tags)
    {
        if (!tags.empty())
        {
            out.U32(sound);
            WriteTags(out, tags, "sound " + std::to_string(sound) + "'s ");
        }
    }

    return out.Take();
}

// A sound without tags adds nothing.
void ReadTags(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the tags");
    file.tags = ReadTagSet(in, "");
    const uint32_t sounds = in.U32();
    for (uint32_t i = 0; i < sounds; ++i)
    {
        const uint32_t sound = in.U32();
        const std::string whose = "sound " + std::to_string(sound) + "'s ";
        Tags tags = ReadTagSet(in, whose);
        if (file.sound_tags.contains(sound))
        {
            Damaged(whose + "tags come twice");
        }

        if (!tags.empty())
        {
            file.sound_tags.emplace(sound, std::move(tags));
        }
    }
}

// LNTH: the sounds' lengths. Their number, then each sound's: its index, its flags, its loops and two zeros, its length
// and its fade, then the ticks of its loop with the flag of them.
std::vector<uint8_t> LengthsChunk(const File& file)
{
    Out out;
    out.Count(file.lengths.size(), "sounds' lengths");
    for (const auto& [sound, length] : file.lengths)
    {
        if (const std::optional<std::string> problem = LengthProblem(length))
        {
            throw std::invalid_argument("sound " + std::to_string(sound) + "'s lengths " + *problem);
        }

        out.U32(sound);
        out.U8(static_cast<uint8_t>((length.silent ? kSilentSound : 0) | (length.loop ? kLoopTicks : 0)));
        out.U8(length.loops);
        out.U16(0);
        out.U32(length.length);
        out.U32(length.fade);
        if (length.loop)
        {
            out.U32(length.loop->start);
            out.U32(length.loop->end);
        }
    }

    return out.Take();
}

void ReadLengths(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the lengths");
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t sound = in.U32();
        const uint8_t flags = in.U8();
        if ((flags & ~(kSilentSound | kLoopTicks)) != 0)
        {
            Newer("sound lengths with a flag");
        }

        SoundLength length;
        length.silent = (flags & kSilentSound) != 0;
        length.loops = in.U8();
        in.U16();
        length.length = in.U32();
        length.fade = in.U32();
        if ((flags & kLoopTicks) != 0)
        {
            length.loop = LoopTicks{in.U32(), in.U32()};
        }

        const std::string whose = "sound " + std::to_string(sound) + "'s lengths";
        if (const std::optional<std::string> problem = LengthProblem(length))
        {
            Damaged(whose + " " + *problem);
        }

        if (!file.lengths.emplace(sound, length).second)
        {
            Damaged(whose + " come twice");
        }
    }
}

// TMLN: the timelines. Their number, then each one: its sequence, the number of runs of bars and each run's length and
// number of bars, then the number of tracks with hints and each track's hints. A track's hints are its number and three
// zeros, then three lists: the blocks before the song's start, the bars kept in the track and the bars called. Each
// list is its length, then its numbers.
std::vector<uint8_t> TimelinesChunk(const File& file)
{
    if (const std::optional<std::string> problem = TimelinesProblem(file))
    {
        throw std::invalid_argument("the timelines " + *problem);
    }

    Out out;
    const auto list = [&out](const std::vector<uint32_t>& numbers, const std::string& what)
    {
        out.Count(numbers.size(), what);
        for (const uint32_t number : numbers)
        {
            out.U32(number);
        }
    };

    out.Count(file.timelines->size(), "timelines");
    for (const auto& [sequence, hints] : *file.timelines)
    {
        out.U32(sequence);
        out.Count(hints.bars.size(), "runs of bars");
        for (const BarRun& run : hints.bars)
        {
            out.U32(run.length);
            out.U32(run.count);
        }

        out.Count(hints.tracks.size(), "tracks' hints");
        for (const TrackHints& track : hints.tracks)
        {
            out.U8(track.track);
            out.Zeros(3);
            list(track.opening, "blocks before a song's start");
            list(track.kept_inline, "bars kept in a track");
            list(track.called, "bars called");
        }
    }

    return out.Take();
}

// The timelines need the sequences: each names one.
void ReadTimelines(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the timelines");
    const auto list = [&in]
    {
        std::vector<uint32_t> numbers(in.U32());
        for (uint32_t& number : numbers)
        {
            number = in.U32();
        }

        return numbers;
    };

    file.timelines.emplace();
    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t sequence = in.U32();
        ConverterHints hints;
        hints.bars.resize(in.U32());
        for (BarRun& run : hints.bars)
        {
            run.length = in.U32();
            run.count = in.U32();
        }

        hints.tracks.resize(in.U32());
        for (TrackHints& track : hints.tracks)
        {
            track.track = in.U8();
            in.Take(3);
            track.opening = list();
            track.kept_inline = list();
            track.called = list();
        }

        if (!file.timelines->emplace(sequence, std::move(hints)).second)
        {
            Damaged("sequence " + std::to_string(sequence) + " has two timelines");
        }
    }

    if (const std::optional<std::string> problem = TimelinesProblem(file))
    {
        Damaged("the timelines " + *problem);
    }
}

// ENGN: the engine's number, the mixing mode's number and two zeros. When sounds take another mode, their number
// follows, then each sound's index, its mode's number and three zeros. When sounds play on another engine, the number
// of sounds in another mode comes all the same. The number of sounds on another engine follows, then each sound's
// index, its engine's number and three zeros.
std::vector<uint8_t> EngineChunk(const File& file)
{
    Out out;
    out.U8(CodeOf(file.engine));
    out.U8(CodeOf(file.mixing));
    out.Zeros(2);
    if (!file.sound_mixing.empty() || !file.sound_engines.empty())
    {
        out.Count(file.sound_mixing.size(), "sounds in another mixing mode");
        for (const auto& [sound, mixing] : file.sound_mixing)
        {
            out.U32(sound);
            out.U8(CodeOf(mixing));
            out.Zeros(3);
        }
    }

    if (!file.sound_engines.empty())
    {
        out.Count(file.sound_engines.size(), "sounds on another engine");
        for (const auto& [sound, engine] : file.sound_engines)
        {
            out.U32(sound);
            out.U8(CodeOf(engine));
            out.Zeros(3);
        }
    }

    return out.Take();
}

void ReadEngine(std::span<const uint8_t> chunk, File& file)
{
    In in(chunk, "the engine");
    const uint8_t engine = in.U8();
    const uint8_t mixing = in.U8();
    const std::optional<Engine> e = EngineOfCode(engine);
    const std::optional<Mixing> m = MixingOfCode(mixing);
    if (!e)
    {
        Newer("an engine (" + std::to_string(engine) + ")");
    }

    if (!m)
    {
        Newer("a mixing mode (" + std::to_string(mixing) + ")");
    }

    file.engine = *e;
    file.mixing = *m;

    // A chunk of 4 bytes gives every sound the file's mode.
    in.U16();
    if (in.Left() == 0)
    {
        return;
    }

    const uint32_t count = in.U32();
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t sound = in.U32();
        const uint8_t code = in.U8();
        in.Take(3);
        const std::optional<Mixing> mode = MixingOfCode(code);
        if (!mode)
        {
            Newer("a mixing mode (" + std::to_string(code) + ")");
        }

        if (!file.sound_mixing.emplace(sound, *mode).second)
        {
            Damaged("sound " + std::to_string(sound) + "'s mixing mode comes twice");
        }
    }

    // A chunk without the sounds' engines plays every sound on the file's engine.
    if (in.Left() == 0)
    {
        return;
    }

    const uint32_t engines = in.U32();
    for (uint32_t i = 0; i < engines; ++i)
    {
        const uint32_t sound = in.U32();
        const uint8_t code = in.U8();
        in.Take(3);
        const std::optional<Engine> named = EngineOfCode(code);
        if (!named)
        {
            Newer("an engine (" + std::to_string(code) + ")");
        }

        if (!FactsOf(*named).hosts_other_consoles)
        {
            Damaged("sound " + std::to_string(sound) + " plays on the " + EngineWords(*named) + " engine");
        }

        if (!file.sound_engines.emplace(sound, *named).second)
        {
            Damaged("sound " + std::to_string(sound) + "'s engine comes twice");
        }
    }
}

} // namespace file_parts

std::string BusName(std::size_t bus)
{
    return std::string(1, static_cast<char>('A' + bus));
}

std::string EngineWords(Engine engine)
{
    if (engine == Engine::kGba)
    {
        return "GBA's";
    }
    if (engine == Engine::kDse)
    {
        return "DSE's";
    }

    const Driver* driver = DriverOf(engine);
    return driver ? std::string(driver->Facts().engine_name) : std::string(FactsOf(engine).title);
}

} // namespace hokuto::format
