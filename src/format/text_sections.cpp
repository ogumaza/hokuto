// SPDX-License-Identifier: MIT

// The sections of the text form other than the sequences: the tables, the native archive, effects, scenes, tags,
// views and timelines.

#include "format/text_parts.h"

namespace hokuto::format::text_parts
{

namespace
{

// The comment that gives the item ID of item `index` of type `type`.
std::string IdComment(uint32_t type, std::size_t index)
{
    return " # ID " + Hex(ntdlib::sar::ItemId(type, static_cast<uint32_t>(index)));
}

// A sound's line: its name and its kind, then each value that differs from the default. Its banks come last.
std::string SoundText(const NativeSound& sound)
{
    const NativeSound kDefaults{};
    std::string text = "sound " + NameText(sound.name) + " " + std::string(NameOf(sound.kind));
    if (sound.player != kDefaults.player)
    {
        text += " player " + Hex(sound.player);
    }

    for (const auto& [name, value] : kSoundBytes)
    {
        if (sound.*value != kDefaults.*value)
        {
            text += " " + std::string(name) + " " + std::to_string(sound.*value);
        }
    }

    if (sound.tracks != kDefaults.tracks)
    {
        text += " tracks " + Hex(sound.tracks);
    }

    if (sound.release_priority_fix)
    {
        text += " release_priority_fix";
    }

    if (!sound.banks.empty())
    {
        text += " banks";
        for (const uint32_t bank : sound.banks)
        {
            text += bank == kNoBank ? " none" : " " + Hex(bank);
        }
    }

    return text;
}

// The native archive's tables: a line for each sound, then for each bank, wave archive, player and group. A comment
// gives each item's ID. Where `game_sizes` give them, each group's line gives the group's game size.
void WriteTables(TextOut& out, const NativeTables& tables, const GameSizes* game_sizes)
{
    out.Line(1, "tables");
    for (std::size_t i = 0; i < tables.sounds.size(); ++i)
    {
        out.Line(2, SoundText(tables.sounds[i]) + IdComment(ntdlib::sar::kSoundItem, i));
    }

    for (std::size_t i = 0; i < tables.banks.size(); ++i)
    {
        out.Line(2, "bank " + NameText(tables.banks[i]) + IdComment(ntdlib::sar::kBankItem, i));
    }

    for (std::size_t i = 0; i < tables.wave_archives.size(); ++i)
    {
        out.Line(2, "wave_archive " + NameText(tables.wave_archives[i]) + IdComment(ntdlib::sar::kWaveArchiveItem, i));
    }

    for (std::size_t i = 0; i < tables.players.size(); ++i)
    {
        const NativePlayer& player = tables.players[i];
        out.Line(2, "player " + NameText(player.name) + " sound_limit " + std::to_string(player.sound_limit) +
                        IdComment(ntdlib::sar::kPlayerItem, i));
    }

    for (std::size_t i = 0; i < tables.groups.size(); ++i)
    {
        const std::string size = game_sizes ? " game_size " + std::to_string(game_sizes->groups[i]) : "";
        out.Line(2, "group " + NameText(tables.groups[i]) + size + IdComment(ntdlib::sar::kGroupItem, i));
    }
}

// A sound's line: sound, its name and its kind, then its values in any order. A value that the line leaves out keeps
// its default. banks takes the numbers and nones that follow it.
NativeSound ParseSound(const std::vector<Token>& tokens, std::size_t line)
{
    if (tokens.size() < 3)
    {
        Fail(line, "a sound's line is sound, its name and its kind, then its values");
    }

    NativeSound sound;
    sound.name = tokens[1].text;
    const std::optional<ntdlib::sar::SoundType> kind = tokens[2].quoted ? std::nullopt : SoundTypeNamed(tokens[2].text);
    if (!kind)
    {
        Fail(line, "there's no sound type named " + tokens[2].text);
    }

    sound.kind = *kind;
    std::set<std::string> given;
    std::size_t t = 3;
    while (t < tokens.size())
    {
        const Token& key = tokens[t++];
        if (!given.insert(key.text).second)
        {
            Fail(line, "the sound sets " + key.text + " twice");
        }

        // The number after the key.
        const auto number = [&](uint64_t max)
        {
            if (t >= tokens.size())
            {
                Fail(line, key.text + " has to be a number");
            }

            return ParseInteger(tokens[t++], max, line, key.text.c_str());
        };

        const auto byte = std::find_if(kSoundBytes.begin(), kSoundBytes.end(),
                                       [&](const auto& entry) { return key.Is(entry.first); });
        if (byte != kSoundBytes.end())
        {
            sound.*(byte->second) = static_cast<uint8_t>(number(0xFF));
        }
        else if (key.Is("player"))
        {
            sound.player = static_cast<uint32_t>(number(0xFFFFFFFF));
        }
        else if (key.Is("tracks"))
        {
            sound.tracks = static_cast<uint32_t>(number(0xFFFFFFFF));
        }
        else if (key.Is("release_priority_fix"))
        {
            sound.release_priority_fix = true;
        }
        else if (key.Is("banks"))
        {
            while (t < tokens.size() && (tokens[t].Is("none") || (!tokens[t].quoted && Integer(tokens[t].text))))
            {
                const Token& bank = tokens[t++];
                sound.banks.push_back(
                    bank.Is("none") ? kNoBank
                                    : static_cast<uint32_t>(ParseInteger(bank, 0xFFFFFFFF, line, "a bank's item ID")));
            }
        }
        else
        {
            Fail(line, "a sound has no value named " + key.text);
        }
    }

    return sound;
}

} // namespace

// Whether `word` starts a line of the native archive's tables.
bool IsTablesWord(const Token& word)
{
    return word.Is("sound") || word.Is("bank") || word.Is("wave_archive") || word.Is("player") || word.Is("group");
}

// A line of the native archive's tables: a sound, a bank, a wave archive, a player or a group. A group's line gives the
// group's game size when the native archive has `game_sizes`, and only then.
void ParseTablesLine(const std::vector<Token>& tokens, std::size_t line, NativeTables& tables,
                     std::optional<GameSizes>& game_sizes)
{
    const Token& word = tokens[0];
    if (word.Is("sound"))
    {
        tables.sounds.push_back(ParseSound(tokens, line));
    }
    else if (word.Is("group") && tokens.size() == 4 && tokens[2].Is("game_size"))
    {
        if (!game_sizes)
        {
            Fail(line, "a group's game_size requires the native archive's game_size line, before the tables");
        }

        tables.groups.push_back(tokens[1].text);
        game_sizes->groups.push_back(ParseInteger(tokens[3], UINT64_MAX, line, "a size"));
    }
    else if (word.Is("group") && tokens.size() == 2 && game_sizes)
    {
        Fail(line, "group " + tokens[1].text + " has no game_size, but the native archive has one");
    }
    else if (word.Is("player"))
    {
        if (tokens.size() != 4 || !tokens[2].Is("sound_limit"))
        {
            Fail(line, "a player's line is player, its name, sound_limit and a number");
        }

        const auto limit = static_cast<uint32_t>(ParseInteger(tokens[3], 0xFFFFFFFF, line, "a sound limit"));
        tables.players.push_back({tokens[1].text, limit});
    }
    else
    {
        if (tokens.size() != 2)
        {
            Fail(line, "a line of " + word.text + " is " + word.text + " and its name");
        }

        std::vector<std::string>& names = word.Is("bank")    ? tables.banks
                                          : word.Is("group") ? tables.groups
                                                             : tables.wave_archives;
        names.push_back(tokens[1].text);
    }
}

namespace
{

// A place among the pieces of `native`: a piece's number, then + and a byte in hex unless it's the first byte, or item
// and an item. The archive's end is end. A place that names a sequence starts with sequence and the sequence's number.
// An item past a bytecode's first goes by its name in `names`: each sequence's names of its items. A name that reads as
// a number takes quotes.
std::string PlaceText(const ntdlib::sar::Place& place, const NativeArchive& native,
                      const std::vector<std::vector<std::string>>& names)
{
    const std::size_t pieces = native.pieces->size();
    if (place.item)
    {
        const auto* file = !place.sequence && place.piece < pieces
                               ? std::get_if<ntdlib::sar::SequencePiece>(&(*native.pieces)[place.piece])
                               : nullptr;
        const std::size_t sequence = place.sequence ? place.piece : file ? file->sequence : names.size();
        const std::vector<std::string>* items = sequence < names.size() ? &names[sequence] : nullptr;
        std::string item = std::to_string(place.offset);
        if (place.offset > 0 && items && place.offset < items->size() && !(*items)[place.offset].empty())
        {
            item = (*items)[place.offset];
            item = Integer(item) ? "\"" + item + "\"" : item;
        }

        return (place.sequence ? "sequence " : "") + std::to_string(place.piece) + " item " + item;
    }

    const std::string piece = place.piece == pieces ? "end" : std::to_string(place.piece);
    return place.offset == 0 ? piece : piece + "+" + Hex(place.offset);
}

} // namespace

// The native archive's game size, the game's build of NW4R SND on the Wii, its pieces, each with its number in a
// comment, then its fields and its tables.
void WriteNative(TextOut& out, const NativeArchive& native, const std::vector<std::vector<std::string>>& names)
{
    if (const std::optional<std::string> problem = GameSizesProblem(native))
    {
        throw std::invalid_argument("the game's sizes " + *problem);
    }

    // The binary form refuses the same archives (NativeChunk).
    (void)WrittenDriverOf(native);

    out.Line(0, "native " + std::string(NameOf(native.engine)));
    if (native.game_sizes)
    {
        out.Line(1, "game_size " + std::to_string(native.game_sizes->archive));
    }

    if (native.driver)
    {
        out.Line(1, "revision " + std::to_string(static_cast<int>(native.driver->revision)));
        out.Line(1,
                 "free_space " + std::to_string(native.driver->free_start) + " padding " + Hex(native.driver->padding));
        for (const FillerRun& run : native.driver->filler)
        {
            out.Line(1, "filler_run " + std::to_string(run.start) + " " + std::to_string(run.size));
        }

        for (const DriverRoom& room : native.driver->rooms)
        {
            out.Line(1, "list_room " + std::to_string(room.piece) + " " + std::to_string(room.end) + " fill " +
                            Hex(room.fill));
        }
    }

    if (native.gba_filler)
    {
        if (native.engine != Engine::kGba && !native.driver)
        {
            throw std::invalid_argument("only a GBA ROM's songs can go into filler runs");
        }

        out.Line(1, "filler");
    }

    if (native.gba_note_arguments != 3)
    {
        if (native.engine != Engine::kGba || native.gba_note_arguments != 2)
        {
            throw std::invalid_argument("only a GBA ROM with MusicPlayer2000 holds notes of 2 argument bytes");
        }

        out.Line(1, "note_arguments 2");
    }

    if (native.wii_build)
    {
        if (native.engine != Engine::kWii)
        {
            throw std::invalid_argument("a build of the Wii's sound library belongs to an archive of the Wii");
        }

        out.Line(1, "build " + std::to_string(native.wii_build->date) +
                        (native.wii_build->october_2006 ? " october_2006" : ""));
    }

    for (std::size_t p = 0; p < native.pieces->size(); ++p)
    {
        const ntdlib::sar::Piece& piece = (*native.pieces)[p];
        const std::string number = " # piece " + std::to_string(p);
        if (const auto* run = std::get_if<std::vector<uint8_t>>(&piece))
        {
            out.Line(1, "bytes " + std::to_string(run->size()) + number);
            out.Bytes(2, *run);
        }
        else if (const auto* file = std::get_if<ntdlib::sar::SequencePiece>(&piece))
        {
            out.Line(1, "sequence_file " + std::to_string(file->sequence) +
                            (native.engine == Engine::kGba || native.driver ? " room " + std::to_string(file->room)
                                                                            : " version " + Hex(file->version)) +
                            number);
        }
        else
        {
            out.Line(1, "padding " + std::to_string(std::get<ntdlib::sar::Padding>(piece).alignment) + number);
        }
    }

    for (const ntdlib::sar::Field& field : native.fields)
    {
        out.Line(1, "field " + PlaceText(field.at, native, names) + " from " + PlaceText(field.from, native, names) +
                        " to " + PlaceText(field.to, native, names) + (field.bytes == 2 ? " bytes 2" : ""));
    }

    if (native.tables)
    {
        WriteTables(out, *native.tables, native.game_sizes ? &*native.game_sizes : nullptr);
    }
}

// An effect's line: effect, the bus, the kind, then each parameter's name and value.
std::string EffectText(std::size_t bus, const Effect& effect)
{
    if (const std::optional<std::string> problem = EffectProblem(effect))
    {
        throw std::invalid_argument("an effect on aux bus " + BusName(bus) + " " + *problem);
    }

    std::string text = "effect " + BusName(bus) + " " + std::string(NameOf(effect.kind));
    const std::span<const std::string_view> names = ParameterNamesOf(effect.kind);
    for (std::size_t p = 0; p < names.size(); ++p)
    {
        text += " " + std::string(names[p]) + " " + FloatText(effect.parameters[p]);
    }

    return text;
}

namespace
{

// A parameter's value: a decimal number with an optional '-', point and exponent, such as -1000, 0.8 or 1e-45, held to
// the nearest float. xsflib also reads a '+' and a decimal comma. The text form takes neither.
float ParseFloat(const Token& token, std::size_t line)
{
    // A value below 2^128 - 2^103 in size rounds to a finite float. A larger one rounds to infinity.
    constexpr double kOverflow = 0x1p128 - 0x1p103;
    const std::string& text = token.text;
    const bool plain = !token.quoted && !text.starts_with('+') && text.find(',') == std::string::npos;
    const std::optional<xsflib::DecimalNumber> number = plain ? xsflib::ReadDecimal(text) : std::nullopt;
    if (!number || number->length != text.size() || !(std::abs(number->value) < kOverflow))
    {
        Fail(line, "a parameter's value is a decimal number within a float's range, not " + text);
    }

    return static_cast<float>(number->value);
}

// The aux bus that `token` names: a letter from A to C. Nothing for another token.
std::optional<std::size_t> BusOf(const Token& token)
{
    const std::string& text = token.text;
    if (token.quoted || text.size() != 1 || text[0] < 'A' || text[0] >= static_cast<char>('A' + kAuxBuses))
    {
        return std::nullopt;
    }

    return static_cast<std::size_t>(text[0] - 'A');
}

} // namespace

// An effect's line: effect, a bus from A to C and a kind of effect, then its parameters' names and values in any order.
// A parameter that the line leaves out is 0. nn::audio starts each parameter at 0. Returns the bus and the effect.
std::pair<std::size_t, Effect> ParseEffect(const std::vector<Token>& tokens, std::size_t line)
{
    const std::optional<std::size_t> bus = tokens.size() >= 3 ? BusOf(tokens[1]) : std::nullopt;
    if (!bus)
    {
        Fail(line, "an effect's line is effect, a bus from A to C and a kind of effect, then its parameters");
    }

    const std::optional<EffectKind> kind = tokens[2].quoted ? std::nullopt : EffectKindNamed(tokens[2].text);
    if (!kind)
    {
        Fail(line, "there's no effect named " + tokens[2].text);
    }

    Effect effect;
    effect.kind = *kind;
    const std::span<const std::string_view> names = ParameterNamesOf(*kind);
    effect.parameters.assign(names.size(), 0.0f);
    std::vector<bool> given(names.size(), false);
    for (std::size_t t = 3; t < tokens.size(); t += 2)
    {
        const Token& name = tokens[t];
        const auto found = std::find(names.begin(), names.end(), name.text);
        if (name.quoted || found == names.end())
        {
            Fail(line, "the effect has no parameter named " + name.text);
        }

        const auto p = static_cast<std::size_t>(found - names.begin());
        if (given[p])
        {
            Fail(line, "the effect sets " + name.text + " twice");
        }

        if (t + 1 >= tokens.size())
        {
            Fail(line, name.text + " has to be a number");
        }

        effect.parameters[p] = ParseFloat(tokens[t + 1], line);
        given[p] = true;
    }

    if (const std::optional<std::string> problem = EffectProblem(effect))
    {
        Fail(line, "the effect " + *problem);
    }

    return {*bus, effect};
}

// An aux bus's volume's line: bus_volume, the bus and the volume as a gain.
std::string BusVolumeText(std::size_t bus, float volume)
{
    const std::string name = BusName(bus);
    if (const std::optional<std::string> problem = AuxVolumeProblem(volume))
    {
        throw std::invalid_argument("aux bus " + name + "'s volume " + *problem);
    }

    return "bus_volume " + name + " " + FloatText(volume);
}

// An aux bus's volume's line: bus_volume, a bus from A to C and the volume. Returns the bus and the volume.
std::pair<std::size_t, float> ParseBusVolume(const std::vector<Token>& tokens, std::size_t line)
{
    const std::optional<std::size_t> bus = tokens.size() == 3 ? BusOf(tokens[1]) : std::nullopt;
    if (!bus)
    {
        Fail(line, "a bus's volume's line is bus_volume, a bus from A to C and the volume");
    }

    const float volume = ParseFloat(tokens[2], line);
    if (const std::optional<std::string> problem = AuxVolumeProblem(volume))
    {
        Fail(line, "the volume " + *problem);
    }

    return {*bus, volume};
}

namespace
{

// A time in milliseconds as seconds, with a fraction only where it has one: 90000 is 90, and 1500 is 1.5.
std::string TimeText(uint32_t time)
{
    std::string text = std::to_string(time / 1000);
    if (const uint32_t fraction = time % 1000; fraction != 0)
    {
        std::string digits = std::to_string(1000 + fraction).substr(1);
        digits.erase(digits.find_last_not_of('0') + 1);
        text += "." + digits;
    }

    return text;
}

// The comment that names sound `sound` at the end of a line: " # " and the sound's name where `tables` name it, or else
// nothing.
std::string SoundNameComment(std::size_t sound, const NativeTables* tables)
{
    if (tables == nullptr || sound >= tables->sounds.size() || tables->sounds[sound].name.empty())
    {
        return {};
    }

    return " # " + NameText(tables->sounds[sound].name);
}

} // namespace

// The lines of scene `index`: its sound and its name, then a line for each starting value and for each change. Where
// `tables` name the sound, a comment gives its name.
void WriteScene(TextOut& out, std::size_t index, const Scene& scene, const NativeTables* tables)
{
    if (const std::optional<std::string> problem = SceneProblem(scene))
    {
        throw std::invalid_argument("scene " + std::to_string(index) + " " + *problem);
    }

    std::string line = "scene " + std::to_string(scene.sound) + " " + NameText(scene.name);
    line += SoundNameComment(scene.sound, tables);
    out.Line(0, line);
    for (const auto& [variable, value] : scene.values)
    {
        out.Line(1, "variable " + std::to_string(variable) + " " + std::to_string(value));
    }

    for (const VariableChange& change : scene.changes)
    {
        out.Line(1, "at " + TimeText(change.time) + " variable " + std::to_string(change.variable) + " " +
                        std::to_string(change.value));
    }
}

// The line of a tag: tag, the key and the value. A sound's tag starts with sound_tag and the sound's number instead.
// Where `tables` name the sound, a comment gives its name.
std::string TagText(std::optional<uint32_t> sound, const std::string& key, const std::string& value,
                    const NativeTables* tables)
{
    if (const std::optional<std::string> problem = TagProblem(key, value))
    {
        throw std::invalid_argument((sound ? "sound " + std::to_string(*sound) + "'s " : std::string()) + *problem);
    }

    std::string line =
        (sound ? "sound_tag " + std::to_string(*sound) + " " : std::string("tag ")) + key + " " + NameText(value);
    if (sound)
    {
        line += SoundNameComment(*sound, tables);
    }

    return line;
}

// The line of a sound's mixing mode: sound_mixing, the sound and the mode's name. Where `tables` name the sound, a
// comment gives its name.
std::string SoundMixingText(uint32_t sound, Mixing mixing, const NativeTables* tables)
{
    std::string line = "sound_mixing " + std::to_string(sound) + " " + std::string(NameOf(mixing));
    line += SoundNameComment(sound, tables);

    return line;
}

// The line of a sound's engine: sound_engine, the sound and the engine's name. Where `tables` name the sound, a comment
// gives its name.
std::string SoundEngineText(uint32_t sound, Engine engine, const NativeTables* tables)
{
    std::string line = "sound_engine " + std::to_string(sound) + " " + std::string(NameOf(engine));
    line += SoundNameComment(sound, tables);

    return line;
}

// The line of a sound's lengths: sound_length, the sound, its length and its fade in milliseconds, and its loops, then
// the ticks where its loop starts and ends if it gives them, and silent for a sound that never makes a sound. Where
// `tables` name the sound, a comment gives its name.
std::string LengthText(uint32_t sound, const SoundLength& length, const NativeTables* tables)
{
    if (const std::optional<std::string> problem = LengthProblem(length))
    {
        throw std::invalid_argument("sound " + std::to_string(sound) + "'s lengths " + *problem);
    }

    std::string line = "sound_length " + std::to_string(sound) + " " + std::to_string(length.length) + " " +
                       std::to_string(length.fade) + " " + std::to_string(length.loops);
    if (length.loop)
    {
        line += " " + std::to_string(length.loop->start) + " " + std::to_string(length.loop->end);
    }

    if (length.silent)
    {
        line += " silent";
    }

    line += SoundNameComment(sound, tables);

    return line;
}

// The line of a track's colour: the sound, the track, and the colour's red, green and blue. Where `tables` name the
// sound, a comment gives its name.
std::string TrackColourText(const TrackPlace& place, const Colour& colour, const NativeTables* tables)
{
    std::string line = "track_colour " + std::to_string(place.sound) + " " + std::to_string(place.track) + " " +
                       std::to_string(colour.red) + " " + std::to_string(colour.green) + " " +
                       std::to_string(colour.blue);
    line += SoundNameComment(place.sound, tables);

    return line;
}

// The lines of the timelines of `file`: a line of each timeline's bars, then a line of each track's hints. A file that
// says that it has no timelines gets a line of none.
void WriteTimelines(TextOut& out, const File& file)
{
    if (const std::optional<std::string> problem = TimelinesProblem(file))
    {
        throw std::invalid_argument("the timelines " + *problem);
    }

    if (file.timelines->empty())
    {
        out.Line(0, "timeline none");
    }

    const auto list = [](const char* word, const std::vector<uint32_t>& numbers)
    {
        std::string text;
        if (!numbers.empty())
        {
            text = std::string(" ") + word;
        }

        for (const uint32_t number : numbers)
        {
            text += " " + std::to_string(number);
        }

        return text;
    };

    for (const auto& [sequence, hints] : *file.timelines)
    {
        const std::string start = "timeline " + std::to_string(sequence);
        std::string line = start + " bars";
        for (const BarRun& run : hints.bars)
        {
            line += " " + std::to_string(run.length) + " " + std::to_string(run.count);
        }

        out.Line(0, line);
        for (const TrackHints& track : hints.tracks)
        {
            out.Line(0, start + " track " + std::to_string(track.track) + list("opening", track.opening) +
                            list("kept", track.kept_inline) + list("called", track.called));
        }
    }
}

namespace
{

// The names of the windows that a view can put in front, by their numbers in ViewFront.
constexpr std::array<std::string_view, 3> kFrontNames = {"mixer", "key_editor", "list_editor"};

} // namespace

// The lines of `view`: the sound, the position, the cycle, the arrangement's zoom and scroll, its selected, muted and
// soloed tracks, the Key Editor's zoom, scroll and lanes, the editors' tracks and the window in front. Where `tables`
// name the sound, a comment gives its name.
void WriteView(TextOut& out, const EditorView& view, const NativeTables* tables)
{
    if (const std::optional<std::string> problem = ViewProblem(view))
    {
        throw std::invalid_argument("the view " + *problem);
    }

    std::string sound = "view sound " + std::to_string(view.sound);
    sound += SoundNameComment(view.sound, tables);

    const auto tracks = [](uint32_t set)
    {
        std::string text;
        for (uint32_t track = 0; track <= kLastViewTrack; ++track)
        {
            if ((set >> track & 1) != 0)
            {
                text += " " + std::to_string(track);
            }
        }

        return text;
    };

    const auto pair = [](float first, float second)
    {
        return FloatText(first) + " " + FloatText(second);
    };

    out.Line(0, sound);
    out.Line(0, "view position " + FloatText(view.position));
    out.Line(0, "view cycle " + pair(view.cycle_start, view.cycle_end) + (view.cycle ? " on" : " off"));
    out.Line(0, "view zoom " + pair(view.beat_width, view.track_height));
    out.Line(0, "view scroll " + pair(view.left, view.top));
    if (view.selected_track)
    {
        out.Line(0, "view track " + std::to_string(*view.selected_track));
    }

    if (view.muted != 0)
    {
        out.Line(0, "view mute" + tracks(view.muted));
    }

    if (view.soloed != 0)
    {
        out.Line(0, "view solo" + tracks(view.soloed));
    }

    out.Line(0, "view key_zoom " + pair(view.key_beat_width, view.key_height));
    out.Line(0, "view key_scroll " + pair(view.key_left, view.key_top));
    for (const ViewLane& lane : view.lanes)
    {
        out.Line(0, "view lane " + std::string(ntdlib::commands::Name(lane.kind)) + " " + std::to_string(lane.index));
    }

    if (view.key_editor_track)
    {
        out.Line(0, "view key_editor " + std::to_string(*view.key_editor_track));
    }

    if (view.list_editor_track)
    {
        out.Line(0, "view list_editor " + std::to_string(*view.list_editor_track));
    }

    out.Line(0, "view front " + std::string(kFrontNames[static_cast<std::size_t>(view.front)]));
}

// A line of the view: view, the name of a part and the part's values. The line's values go into `view`. `given` holds
// the parts that earlier lines gave. Each part comes once, but a lane can come more than once.
void ParseView(const std::vector<Token>& tokens, std::size_t line, EditorView& view, std::set<std::string>& given)
{
    if (tokens.size() < 2 || tokens[1].quoted)
    {
        Fail(line, "a line of the view is view, the name of a part and its values");
    }

    const std::string& part = tokens[1].text;
    if (part != "lane" && !given.insert(part).second)
    {
        Fail(line, "the view sets its " + part + " twice");
    }

    const auto takes = [&tokens, &part, line](std::size_t values, const std::string& what)
    {
        if (tokens.size() != values + 2)
        {
            Fail(line, "the view's " + part + " is " + what);
        }
    };

    const auto track = [line](const Token& token)
    {
        return static_cast<uint8_t>(ParseInteger(token, kLastViewTrack, line, "a track's number"));
    };

    if (part == "sound")
    {
        takes(1, "a sound's number");
        view.sound = static_cast<uint32_t>(ParseInteger(tokens[2], 0xFFFFFFFF, line, "a sound's number"));
    }
    else if (part == "position")
    {
        takes(1, "a position in quarter notes");
        view.position = ParseFloat(tokens[2], line);
    }
    else if (part == "cycle")
    {
        takes(3, "its start, its end, and on or off");
        if (!tokens[4].Is("on") && !tokens[4].Is("off"))
        {
            Fail(line, "the cycle is on or off, not " + tokens[4].text);
        }

        view.cycle_start = ParseFloat(tokens[2], line);
        view.cycle_end = ParseFloat(tokens[3], line);
        view.cycle = tokens[4].Is("on");
    }
    else if (part == "zoom")
    {
        takes(2, "the width of a beat and the height of a track");
        view.beat_width = ParseFloat(tokens[2], line);
        view.track_height = ParseFloat(tokens[3], line);
    }
    else if (part == "scroll")
    {
        takes(2, "the quarter note at the left edge and the track at the top");
        view.left = ParseFloat(tokens[2], line);
        view.top = ParseFloat(tokens[3], line);
    }
    else if (part == "key_zoom")
    {
        takes(2, "the width of a beat and the height of a key");
        view.key_beat_width = ParseFloat(tokens[2], line);
        view.key_height = ParseFloat(tokens[3], line);
    }
    else if (part == "key_scroll")
    {
        takes(2, "the quarter note at the left edge and the key at the top");
        view.key_left = ParseFloat(tokens[2], line);
        view.key_top = ParseFloat(tokens[3], line);
    }
    else if (part == "track" || part == "key_editor" || part == "list_editor")
    {
        takes(1, "a track's number");
        std::optional<uint8_t>& number = part == "track"        ? view.selected_track
                                         : part == "key_editor" ? view.key_editor_track
                                                                : view.list_editor_track;
        number = track(tokens[2]);
    }
    else if (part == "mute" || part == "solo")
    {
        if (tokens.size() < 3)
        {
            Fail(line, "the view's " + part + " has to list the numbers of tracks");
        }

        uint32_t& set = part == "mute" ? view.muted : view.soloed;
        for (std::size_t t = 2; t < tokens.size(); ++t)
        {
            set |= uint32_t{1} << track(tokens[t]);
        }
    }
    else if (part == "front")
    {
        takes(1, "mixer, key_editor or list_editor");
        const auto front = std::ranges::find(kFrontNames, tokens[2].quoted ? "" : tokens[2].text);
        if (front == kFrontNames.end())
        {
            Fail(line, "the window in front is mixer, key_editor or list_editor, not " + tokens[2].text);
        }

        view.front = static_cast<ViewFront>(front - kFrontNames.begin());
    }
    else if (part == "lane")
    {
        takes(2, "a kind of command, and a bus or an LFO");
        const std::optional<Kind> kind = tokens[2].quoted ? std::nullopt : ntdlib::commands::KindNamed(tokens[2].text);
        if (!kind)
        {
            Fail(line, "there's no command type named " + tokens[2].text);
        }

        view.lanes.push_back({*kind, static_cast<uint8_t>(ParseInteger(tokens[3], 0xFF, line, "a bus or an LFO"))});
    }
    else
    {
        Fail(line, "the view has no part named " + part);
    }
}

namespace
{

// A time in seconds, to the millisecond: a whole number of seconds, with three decimal places at most.
uint32_t ParseTime(const Token& token, std::size_t line)
{
    const std::string_view text = token.text;
    const std::size_t point = text.find('.');
    const std::string_view whole = text.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view() : text.substr(point + 1);

    const auto digits = [](std::string_view part)
    {
        return std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    const bool plain = !token.quoted && !whole.empty() && whole.size() <= 7 && digits(whole) && digits(fraction) &&
                       fraction.size() <= 3 && (point == std::string_view::npos || !fraction.empty());
    uint64_t time = 0;
    if (plain)
    {
        for (const char c : whole)
        {
            time = time * 10 + static_cast<uint64_t>(c - '0');
        }

        for (std::size_t d = 0; d < 3; ++d)
        {
            time = time * 10 + (d < fraction.size() ? static_cast<uint64_t>(fraction[d] - '0') : 0);
        }
    }
    if (!plain || time > UINT32_MAX)
    {
        Fail(line, "a time is a number of seconds from 0 to 4294967.295, with three decimal places at most, not " +
                       token.text);
    }

    return static_cast<uint32_t>(time);
}

// A variable's value: a whole number from -32768 to 32767.
int16_t ParseVariableValue(const Token& token, std::size_t line)
{
    const std::string_view text = token.text;
    const bool negative = text.starts_with('-');
    const std::optional<uint64_t> size = token.quoted ? std::nullopt : Integer(text.substr(negative ? 1 : 0));
    if (!size || *size > (negative ? 32768u : 32767u))
    {
        Fail(line, "a variable's value is a whole number from -32768 to 32767, not " + token.text);
    }

    const auto magnitude = static_cast<int32_t>(*size);
    return static_cast<int16_t>(negative ? -magnitude : magnitude);
}

} // namespace

// A line of a scene's values: variable, a variable's number and the value that it starts with, or at and a time before
// them for a change at that time. A scene starts each variable once at most. Its changes come in the order of their
// times.
void ParseSceneValue(const std::vector<Token>& tokens, std::size_t line, Scene& scene)
{
    const bool change = tokens[0].Is("at");
    const std::size_t first = change ? 2 : 0;
    if (tokens.size() != first + 3 || !tokens[first].Is("variable"))
    {
        Fail(line, "a scene's line is variable, a variable's number and its value, or at and a time before them");
    }

    const auto variable =
        static_cast<uint8_t>(ParseInteger(tokens[first + 1], kSceneVariables - 1, line, "a variable's number"));
    const int16_t value = ParseVariableValue(tokens[first + 2], line);
    if (!change)
    {
        if (!scene.values.emplace(variable, value).second)
        {
            Fail(line, "the scene starts variable " + std::to_string(variable) + " twice");
        }

        return;
    }

    const uint32_t time = ParseTime(tokens[1], line);
    if (!scene.changes.empty() && time < scene.changes.back().time)
    {
        Fail(line, "a scene's changes come in the order of their times");
    }

    scene.changes.push_back({time, variable, value});
}

namespace
{

// A place of a field's line, with the label that names its item, if a label does.
struct PlaceDraft
{
    ntdlib::sar::Place place;
    std::optional<Token> label;
};

// The place at token `t` of a field's line, and the token after it: a piece's number or end, with + and a byte, a
// piece's number, item, and an item's number or label, or sequence, a sequence's number, item, and an item's number or
// label.
PlaceDraft ParsePlace(const std::vector<Token>& tokens, std::size_t& t, std::size_t line)
{
    const std::string kForm =
        "a place is a piece's number or end, with + and a byte, a piece's number, item, and an item's number or label, "
        "or sequence, a sequence's number, item, and an item's number or label";
    const bool sequence = t < tokens.size() && tokens[t].Is("sequence");
    t += sequence ? 1 : 0;
    if (t >= tokens.size() || tokens[t].quoted)
    {
        Fail(line, kForm);
    }

    const std::string_view text = tokens[t++].text;
    const std::size_t plus = text.find('+');
    const std::string_view piece = text.substr(0, plus);
    const std::optional<uint64_t> number = piece == "end" ? std::optional<uint64_t>(kEndPiece) : Integer(piece);
    const std::optional<uint64_t> offset =
        plus == std::string_view::npos ? std::optional<uint64_t>(0) : Integer(text.substr(plus + 1));
    if (!number || *number > kEndPiece || (piece != "end" && *number == kEndPiece) || !offset || *offset > 0xFFFFFFFF)
    {
        Fail(line, kForm + ", not " + std::string(text));
    }

    PlaceDraft draft{{static_cast<uint32_t>(*number), static_cast<uint32_t>(*offset), false, sequence}, std::nullopt};
    if (sequence && (plus != std::string_view::npos || draft.place.piece == kEndPiece ||
                     !(t < tokens.size() && tokens[t].Is("item"))))
    {
        Fail(line, kForm);
    }

    if (t < tokens.size() && tokens[t].Is("item"))
    {
        if (plus != std::string_view::npos || draft.place.piece == kEndPiece || t + 1 >= tokens.size())
        {
            Fail(line, kForm);
        }

        // An item's number, or a label. A number in quotes is a label's name.
        const Token& item = tokens[t + 1];
        draft.place.item = true;
        if (!item.quoted && Integer(item.text))
        {
            draft.place.offset = static_cast<uint32_t>(ParseInteger(item, 0xFFFFFFFF, line, "an item"));
        }
        else
        {
            draft.label = item;
        }

        t += 2;
    }

    return draft;
}

} // namespace

// A field's line: field, where the field lies, from and a place, then to and a place. A field of 16 bits, in a set of
// DSE files, ends with bytes 2.
FieldDraft ParseField(const std::vector<Token>& tokens, std::size_t line)
{
    const std::string kForm = "a field's line is field and where it lies, from and a place, then to and a place";
    std::size_t t = 1;
    FieldDraft draft;
    draft.line = line;
    const PlaceDraft at = ParsePlace(tokens, t, line);
    if (at.label || t >= tokens.size() || !tokens[t].Is("from"))
    {
        Fail(line, kForm);
    }

    const PlaceDraft from = ParsePlace(tokens, ++t, line);
    if (t >= tokens.size() || !tokens[t].Is("to"))
    {
        Fail(line, kForm);
    }

    const PlaceDraft to = ParsePlace(tokens, ++t, line);
    const bool two_bytes = t + 2 == tokens.size() && tokens[t].Is("bytes") && tokens[t + 1].Is("2");
    if (t + (two_bytes ? 2 : 0) != tokens.size())
    {
        Fail(line, kForm);
    }

    draft.field = {at.place, from.place, to.place, static_cast<uint8_t>(two_bytes ? 2 : 4)};
    draft.from_label = from.label;
    draft.to_label = to.label;

    return draft;
}

// The item that `label` names in the sequence that `place` names: the sequence of a piece of `native`, or one of
// `sequences`, the native archive's sequences. `locals` holds each sequence's local labels.
uint32_t ItemOfLabel(const NativeArchive& native, std::span<const Sequence> sequences,
                     const std::vector<std::map<std::string, uint32_t>>& locals, const ntdlib::sar::Place& place,
                     const Token& label, std::size_t line)
{
    // The place names a sequence file, or a sequence.
    uint32_t s = place.piece;
    if (place.sequence && s >= sequences.size())
    {
        Fail(line, "the field names sequence " + std::to_string(s) + ", and the file has " +
                       std::to_string(sequences.size()));
    }

    if (!place.sequence)
    {
        const std::vector<ntdlib::sar::Piece>& pieces = *native.pieces;
        const auto* sequence_file = s < pieces.size() ? std::get_if<ntdlib::sar::SequencePiece>(&pieces[s]) : nullptr;
        if (!sequence_file)
        {
            Fail(line,
                 "the field names an item of piece " + std::to_string(s) + ", and that piece isn't a sequence file");
        }

        s = sequence_file->sequence;
    }

    if (!label.quoted && label.text.starts_with("."))
    {
        const auto found = locals[s].find(label.text);
        if (found != locals[s].end())
        {
            return found->second;
        }
    }
    else
    {
        for (const ntdlib::commands::Label& named : sequences[s].labels)
        {
            if (named.name == label.text)
            {
                return named.item;
            }
        }
    }

    Fail(line, "sequence " + std::to_string(s) + " has no label named " + label.text);
}

namespace
{

// The hints of a track: its number at token 3, then each list after its word.
TrackHints ParseTrackHints(const std::vector<Token>& tokens, std::size_t number)
{
    TrackHints track;
    track.track = static_cast<uint8_t>(ParseInteger(tokens[3], 0xFF, number, "a track's number"));
    std::vector<uint32_t>* list = nullptr;
    std::set<std::string> given;
    for (std::size_t t = 4; t < tokens.size(); ++t)
    {
        if (tokens[t].Is("opening") || tokens[t].Is("kept") || tokens[t].Is("called"))
        {
            if (!given.insert(tokens[t].text).second)
            {
                Fail(number, "a track's hints have " + tokens[t].text + " once at most");
            }

            list = tokens[t].Is("opening") ? &track.opening : tokens[t].Is("kept") ? &track.kept_inline : &track.called;
            continue;
        }
        if (list == nullptr)
        {
            Fail(number, "a track's numbers follow opening, kept or called");
        }

        list->push_back(static_cast<uint32_t>(ParseInteger(tokens[t], 0xFFFFFFFF, number, "a number of a track")));
    }

    return track;
}

} // namespace

// Reads a timeline's line into the timelines of `file`: none, a timeline's bars, or a track's hints after its
// timeline's bars. `read` takes the line.
void ParseTimeline(const std::vector<Token>& tokens, std::size_t number, File& file, TimelineLines& read)
{
    if (tokens.size() == 2 && tokens[1].Is("none"))
    {
        if (file.timelines)
        {
            Fail(number, "a file with timelines or a line of none can't have a line of none");
        }

        file.timelines.emplace();
        read.none = number;
        return;
    }

    if (tokens.size() < 3 || !(tokens[2].Is("bars") || tokens[2].Is("track")))
    {
        Fail(number, "a timeline's line is timeline, its sequence's number, and bars or a track's hints");
    }

    if (read.none != 0)
    {
        Fail(number, "a file with a line of none can't have timelines");
    }

    const auto sequence = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sequence's number"));
    if (!file.timelines)
    {
        file.timelines.emplace();
    }

    if (tokens[2].Is("bars"))
    {
        if (tokens.size() % 2 == 0)
        {
            Fail(number, "a timeline's bars come in pairs: a bar's length in ticks, and the number of bars");
        }

        if (!read.bars.emplace(sequence, number).second)
        {
            Fail(number, "sequence " + std::to_string(sequence) + " already has a timeline");
        }

        ConverterHints& hints = (*file.timelines)[sequence];
        for (std::size_t t = 3; t < tokens.size(); t += 2)
        {
            hints.bars.push_back(
                {static_cast<uint32_t>(ParseInteger(tokens[t], 0xFFFFFFFF, number, "a bar's length")),
                 static_cast<uint32_t>(ParseInteger(tokens[t + 1], 0xFFFFFFFF, number, "a number of bars"))});
        }

        return;
    }

    // A track's hints: its number, then each list after its word.
    const auto found = file.timelines->find(sequence);
    if (found == file.timelines->end() || tokens.size() < 4)
    {
        Fail(number,
             "a track's hints are timeline, the sequence's number, track and the track's number, after the "
             "timeline's bars");
    }

    found->second.tracks.push_back(ParseTrackHints(tokens, number));
}

} // namespace hokuto::format::text_parts
