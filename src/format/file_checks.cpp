// SPDX-License-Identifier: MIT

// The checks of a file's values, the choices that a file's items make, the edits that set a sound's tag, mixing and
// engine, and the CRC-32 of a file.

#include "xsflib/binary.h"

#include "format/engines.h"
#include "format/file_parts.h"

namespace hokuto::format
{
namespace
{

using file_parts::Command;
using file_parts::Sequence;

} // namespace

bool HasChoices(const Command& item)
{
    return HasNwsndChoices(item) || HasMp2kChoices(item);
}

bool HasNwsndChoices(const Command& item)
{
    return item.encoding.value_bytes != 0 || item.encoding.unused_time != 0 || item.encoding.unused_random ||
           item.encoding.unused_variable;
}

bool HasMp2kChoices(const Command& item)
{
    return item.encoding.repeated || item.encoding.arguments || item.encoding.length_code;
}

uint8_t EncodingChoicesOf(const Command& item)
{
    using namespace encoding_choices;
    const ntdlib::commands::Encoding& encoding = item.encoding;
    return static_cast<uint8_t>((encoding.value_bytes != 0 ? kValueBytes : 0) |
                                (encoding.unused_time != 0 ? kUnusedTime : 0) |
                                (encoding.unused_random ? kUnusedRandom : 0) |
                                (encoding.unused_variable ? kUnusedVariable : 0) | (encoding.repeated ? kRepeated : 0) |
                                (encoding.arguments ? kArguments : 0) | (encoding.length_code ? kLengthCode : 0));
}

namespace
{

// A choice that DSE's bytes don't make.
bool HasChoicesOutsideDse(const Command& item)
{
    return item.encoding.unused_time != 0 || item.encoding.unused_random || item.encoding.unused_variable ||
           item.encoding.length_code;
}

} // namespace

std::optional<std::size_t> ForeignChoices(const File& file)
{
    if (!file.native)
    {
        return std::nullopt;
    }

    const Driver* driver = DriverOfFile(file);
    const auto foreign = [&](const Command& item)
    {
        return file.native->engine == Engine::kGba   ? HasNwsndChoices(item)
               : file.native->engine == Engine::kDse ? HasChoicesOutsideDse(item)
               : driver                              ? driver->Foreign(item)
                                                     : HasMp2kChoices(item);
    };
    for (std::size_t s = 0; s < file.sequences.size(); ++s)
    {
        const std::vector<Command>& items = file.sequences[s]->items;
        if (std::any_of(items.begin(), items.end(), foreign))
        {
            return s;
        }
    }

    return std::nullopt;
}

std::optional<std::size_t> UnfiledChoices(const File& file)
{
    std::vector<bool> filed(file.sequences.size());
    const auto file_pieces = [&filed](std::span<const ntdlib::sar::Piece> pieces)
    {
        for (const ntdlib::sar::Piece& piece : pieces)
        {
            const auto* sequence_piece = std::get_if<ntdlib::sar::SequencePiece>(&piece);
            if (sequence_piece && sequence_piece->sequence < filed.size())
            {
                filed[sequence_piece->sequence] = true;
            }
        }
    };
    if (file.native)
    {
        file_pieces(*file.native->pieces);
        for (const NativeGroupFile& group_file : file.native->group_files)
        {
            file_pieces(*group_file.pieces);
        }
    }

    for (std::size_t s = 0; s < file.sequences.size(); ++s)
    {
        const std::vector<Command>& items = file.sequences[s]->items;
        if (!filed[s] && std::any_of(items.begin(), items.end(), HasChoices))
        {
            return s;
        }
    }

    return std::nullopt;
}

std::optional<std::string> EffectProblem(const Effect& effect)
{
    const std::size_t count = ParameterNamesOf(effect.kind).size();
    if (const std::size_t given = effect.parameters.size(); given != count)
    {
        return "has " + std::to_string(given) + (given == 1 ? " parameter" : " parameters") + ", and its kind has " +
               std::to_string(count);
    }

    const auto finite = [](float parameter)
    {
        return std::isfinite(parameter);
    };
    if (!std::all_of(effect.parameters.begin(), effect.parameters.end(), finite))
    {
        return std::string("has a parameter that isn't a finite number");
    }

    // A mode is a whole number from 0 to one less than its choices. A parameter with limits lies within them, a whole
    // number of steps from the lowest value where the parameter moves in steps.
    for (std::size_t p = 0; p < count; ++p)
    {
        const uint32_t choices = ChoicesOf(effect.kind, p);
        const float value = effect.parameters[p];
        const std::string name(ParameterNamesOf(effect.kind)[p]);
        if (choices > 0 && (value < 0.0f || value >= static_cast<float>(choices) || value != std::floor(value)))
        {
            return "has a value of " + name + " that isn't a whole number from 0 to " + std::to_string(choices - 1);
        }

        const std::optional<ParameterLimits> limits = LimitsOf(effect.kind, p);
        if (!limits)
        {
            continue;
        }

        // The limits are whole numbers.
        const auto whole = [](float bound)
        {
            return std::to_string(static_cast<int64_t>(bound));
        };

        const float steps = limits->step > 0.0f ? (value - limits->low) / limits->step : 0.0f;
        if (value < limits->low || value > limits->high || steps != std::floor(steps))
        {
            if (limits->step == 0.0f)
            {
                return "has a value of " + name + " that is outside " + whole(limits->low) + " to " +
                       whole(limits->high);
            }

            const std::string range = " from " + whole(limits->low) + " to " + whole(limits->high);
            if (limits->step == 1.0f)
            {
                return "has a value of " + name + " that isn't a whole number" + range;
            }

            return "has a value of " + name + " that isn't a multiple of " + whole(limits->step) + range;
        }
    }

    return std::nullopt;
}

std::optional<std::string> AuxVolumeProblem(float volume)
{
    if (!std::isfinite(volume))
    {
        return std::string("isn't a finite number");
    }
    if (volume < 0.0f || volume > kMaxAuxVolume)
    {
        return std::string("is outside 0 to 2");
    }

    return std::nullopt;
}

std::optional<std::string> SceneProblem(const Scene& scene)
{
    const auto past = [](int variable) -> std::optional<std::string>
    {
        if (variable < kSceneVariables)
        {
            return std::nullopt;
        }

        return "sets variable " + std::to_string(variable) + ", and a scene sets variables 0 to " +
               std::to_string(kSceneVariables - 1) + " only";
    };
    for (const auto& [variable, value] : scene.values)
    {
        if (std::optional<std::string> problem = past(variable))
        {
            return problem;
        }
    }

    for (const VariableChange& change : scene.changes)
    {
        if (std::optional<std::string> problem = past(change.variable))
        {
            return problem;
        }
    }

    const auto earlier = [](const VariableChange& first, const VariableChange& second)
    {
        return first.time < second.time;
    };
    if (!std::is_sorted(scene.changes.begin(), scene.changes.end(), earlier))
    {
        return std::string("changes its variables out of the order of their times");
    }

    return std::nullopt;
}

std::optional<std::string> GameSizesProblem(const NativeArchive& native)
{
    const std::size_t groups = native.tables ? native.tables->groups.size() : 0;
    if (!native.game_sizes || native.game_sizes->groups.size() == groups)
    {
        return std::nullopt;
    }

    const auto counted = [](std::size_t count)
    {
        return std::to_string(count) + (count == 1 ? " group" : " groups");
    };
    const std::string covered = "cover " + counted(native.game_sizes->groups.size());
    if (!native.tables)
    {
        return covered + ", and the native archive has no tables";
    }

    return covered + ", and the tables list " + counted(groups);
}

File WithTag(File file, std::optional<uint32_t> sound, const std::string& key, const std::string& value)
{
    Tags& tags = sound ? file.sound_tags[*sound] : file.tags;
    if (value.empty())
    {
        tags.erase(key);
    }
    else
    {
        tags[key] = value;
    }
    if (sound && tags.empty())
    {
        file.sound_tags.erase(*sound);
    }

    return file;
}

Mixing MixingOf(const File& file, uint32_t sound)
{
    const auto found = file.sound_mixing.find(sound);
    return found != file.sound_mixing.end() ? found->second : file.mixing;
}

File WithMixing(File file, uint32_t sound, Mixing mixing)
{
    if (mixing == file.mixing)
    {
        file.sound_mixing.erase(sound);
    }
    else
    {
        file.sound_mixing[sound] = mixing;
    }

    return file;
}

File WithMixing(File file, Mixing mixing)
{
    file.mixing = mixing;
    file.sound_mixing.clear();

    return file;
}

std::optional<std::string> MixingRefused(Engine engine, Mixing mixing)
{
    if (FactsOf(engine).strict_only && mixing == Mixing::kEnhanced)
    {
        return "The Wii plays in strict mixing only.";
    }

    return std::nullopt;
}

Engine EngineOf(const File& file, uint32_t sound)
{
    const auto found = file.sound_engines.find(sound);
    return found != file.sound_engines.end() ? found->second : file.engine;
}

File WithEngine(File file, uint32_t sound, Engine engine)
{
    if (engine == file.engine)
    {
        file.sound_engines.erase(sound);
    }
    else
    {
        file.sound_engines[sound] = engine;
    }

    return file;
}

Family FamilyOf(const NativeArchive& native)
{
    return FactsOf(native.engine).family;
}

ntdlib::sar::Flavor FlavorOf(const NativeArchive& native)
{
    return FlavorOf(native.engine).value_or(ntdlib::sar::Flavor::kNx);
}

void CheckNativeEngine(Engine engine, const NativeArchive& native)
{
    if (native.engine != engine)
    {
        throw xsflib::FormatError("This file names two different engines. It may be damaged.");
    }
}

void SetEngine(NativeArchive& native, Engine engine)
{
    if (!IsDriverEngine(engine))
    {
        native.driver.reset();
    }
    else if (!native.driver || native.engine != engine)
    {
        native.driver = NativeDriver();
    }

    native.engine = engine;
}

std::optional<std::string> LengthProblem(const SoundLength& length)
{
    if (length.loop && length.loops == 0)
    {
        return std::string("have loop ticks but a loop count of 0");
    }
    if (length.loop && length.loop->end <= length.loop->start)
    {
        return "have a loop whose end at tick " + std::to_string(length.loop->end) + " isn't after its start at tick " +
               std::to_string(length.loop->start);
    }

    return std::nullopt;
}

File WithoutStaleLengths(const File& before, File after)
{
    const auto shared = [&before, &after]
    {
        for (std::size_t i = 0; i < after.sequences.size(); i++)
        {
            if (!after.sequences[i].SharesWith(before.sequences[i]))
            {
                return false;
            }
        }

        return true;
    };
    if (after.engine != before.engine || after.aux_effects != before.aux_effects ||
        after.aux_volumes != before.aux_volumes || after.sequences.size() != before.sequences.size() || !shared() ||
        !after.native.SharesWith(before.native))
    {
        after.lengths.clear();
    }

    return after;
}

std::optional<std::string> TagProblem(std::string_view key, std::string_view value)
{
    const auto lower = [](char c)
    {
        return c >= 'a' && c <= 'z';
    };
    const auto allowed = [&lower](char c)
    {
        return lower(c) || (c >= '0' && c <= '9') || c == '_';
    };
    if (key.empty())
    {
        return "a tag has no key";
    }
    if (key.size() > kLongestTagKey || !lower(key.front()) || !std::all_of(key.begin(), key.end(), allowed))
    {
        return "tag " + std::string(key) + " has a key other than 1 to " + std::to_string(kLongestTagKey) +
               " lower-case letters, digits and underscores from a letter on";
    }
    if (value.empty())
    {
        return "tag " + std::string(key) + " has no value";
    }

    return std::nullopt;
}

std::optional<std::string> TimelinesProblem(const File& file)
{
    if (!file.timelines)
    {
        return std::nullopt;
    }

    const auto rising = [](const std::vector<uint32_t>& numbers)
    {
        return std::adjacent_find(numbers.begin(), numbers.end(), std::greater_equal<>()) == numbers.end();
    };
    for (const auto& [sequence, hints] : *file.timelines)
    {
        const std::string of = " of sequence " + std::to_string(sequence);
        if (sequence >= file.sequences.size())
        {
            return "name sequence " + std::to_string(sequence) + ", and the file has " +
                   std::to_string(file.sequences.size());
        }

        for (const BarRun& run : hints.bars)
        {
            if (run.length == 0 || run.count == 0)
            {
                return "have a run of bars" + of + " with no ticks or no bars";
            }
        }

        for (std::size_t i = 0; i < hints.tracks.size(); ++i)
        {
            const TrackHints& track = hints.tracks[i];
            if (i > 0 && track.track <= hints.tracks[i - 1].track)
            {
                return "list the track hints" + of + " out of order";
            }
            if (!rising(track.kept_inline) || !rising(track.called))
            {
                return "list the bars of track " + std::to_string(track.track) + of + " out of order";
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> ViewProblem(const EditorView& view)
{
    const std::array<float, 11> numbers = {view.position,     view.cycle_start, view.cycle_end, view.beat_width,
                                           view.track_height, view.left,        view.top,       view.key_beat_width,
                                           view.key_height,   view.key_left,    view.key_top};
    const std::array<float, 4> sizes = {view.beat_width, view.track_height, view.key_beat_width, view.key_height};
    const auto finite = [](float number)
    {
        return std::isfinite(number);
    };
    const auto positive = [](float size)
    {
        return size > 0.0f;
    };
    if (!std::all_of(numbers.begin(), numbers.end(), finite))
    {
        return "contains a number that isn't finite";
    }
    if (!std::all_of(sizes.begin(), sizes.end(), positive))
    {
        return "has a size of 0 or less";
    }

    for (const std::optional<uint8_t> track : {view.selected_track, view.key_editor_track, view.list_editor_track})
    {
        if (track && *track > kLastViewTrack)
        {
            return "names track " + std::to_string(*track) + ", and tracks run from 0 to " +
                   std::to_string(kLastViewTrack);
        }
    }

    return std::nullopt;
}

std::optional<std::string> FieldProblem(const NativeArchive& native, std::span<const Sequence> sequences,
                                        const ntdlib::sar::Field& field)
{
    return FieldProblem(*native.pieces, sequences, field);
}

std::optional<std::string> FieldProblem(std::span<const ntdlib::sar::Piece> pieces, std::span<const Sequence> sequences,
                                        const ntdlib::sar::Field& field)
{
    const auto* at = field.at.piece < pieces.size() && !field.at.item && !field.at.sequence
                         ? std::get_if<std::vector<uint8_t>>(&pieces[field.at.piece])
                         : nullptr;
    if (!at || field.at.offset + std::size_t{field.bytes} > at->size())
    {
        return "is outside a run of bytes";
    }

    // A field can name two items of a sequence without a piece among the pieces, such as a sequence file that only
    // group files hold.
    if (field.from.sequence || field.to.sequence)
    {
        const uint32_t sequence = field.from.piece;
        if (!field.from.sequence || !field.to.sequence || !field.from.item || !field.to.item ||
            field.to.piece != sequence)
        {
            return "names items of a sequence and something else";
        }

        if (sequence >= sequences.size())
        {
            return "names sequence " + std::to_string(sequence) + ", and the file has " +
                   std::to_string(sequences.size());
        }

        for (const ntdlib::sar::Place& place : {field.from, field.to})
        {
            if (place.offset > sequences[sequence].items.size())
            {
                return "names item " + std::to_string(place.offset) + ", past the end of sequence " +
                       std::to_string(sequence);
            }
        }

        return std::nullopt;
    }

    for (const ntdlib::sar::Place& place : {field.from, field.to})
    {
        const std::string piece = "piece " + std::to_string(place.piece);
        if (place.piece > pieces.size())
        {
            return "names " + piece + ", and the archive has " + std::to_string(pieces.size());
        }

        if (place.item)
        {
            const auto* file =
                place.piece < pieces.size() ? std::get_if<ntdlib::sar::SequencePiece>(&pieces[place.piece]) : nullptr;
            if (!file)
            {
                return "names an item of " + piece + ", and that piece isn't a sequence file";
            }
            if (file->sequence < sequences.size() && place.offset > sequences[file->sequence].items.size())
            {
                return "names item " + std::to_string(place.offset) + ", past the end of sequence " +
                       std::to_string(file->sequence);
            }

            continue;
        }

        const auto* run =
            place.piece < pieces.size() ? std::get_if<std::vector<uint8_t>>(&pieces[place.piece]) : nullptr;
        if (place.offset != 0 && place.piece < pieces.size() && (!run || place.offset >= run->size()))
        {
            return "names byte " + std::to_string(place.offset) + " of " + piece + ", outside its bytes";
        }
    }

    return std::nullopt;
}

uint32_t Crc32(std::span<const uint8_t> bytes)
{
    // The reflected polynomial 0x04C11DB7, a byte at a time.
    static const std::array<uint32_t, 256> kTable = []
    {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }

            table[i] = c;
        }

        return table;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes)
    {
        crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFFu;
}

} // namespace hokuto::format
