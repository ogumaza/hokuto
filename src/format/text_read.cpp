// SPDX-License-Identifier: MIT

// Reads a file of Hokuto in the text form.

#include <algorithm>
#include <array>
#include <bit>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/dse/commands.h"
#include "ntdlib/mp2k/commands.h"
#include "ntdlib/ntr/snd/commands.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

#include "format/binary.h"
#include "format/codes.h"
#include "format/command_values.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"
#include "format/text.h"
#include "format/text_parts.h"

namespace hokuto::format
{
namespace
{

using text_parts::Command;
using text_parts::CommandProblem;
using text_parts::DecodeBase64;
using text_parts::DriverProblem;
using text_parts::DseProblem;
using text_parts::Fail;
using text_parts::FieldDraft;
using text_parts::FinishSequence;
using text_parts::Integer;
using text_parts::IsTablesWord;
using text_parts::ItemOfLabel;
using text_parts::kEndPiece;
using text_parts::Kind;
using text_parts::Mp2kProblem;
using text_parts::NtrProblem;
using text_parts::ParseBusVolume;
using text_parts::ParseCommand;
using text_parts::ParseEffect;
using text_parts::ParseField;
using text_parts::ParseInteger;
using text_parts::ParseSceneValue;
using text_parts::ParseTablesLine;
using text_parts::ParseTimeline;
using text_parts::ParseView;
using text_parts::Sequence;
using text_parts::SequenceDraft;
using text_parts::TimelineLines;
using text_parts::Token;
using text_parts::Tokenize;

// Runs `read`. A FormatError that it throws has the number of line `line` before its message.
template <typename Read>
void AtLine(std::size_t line, Read read)
{
    try
    {
        read();
    }
    catch (const xsflib::FormatError& error)
    {
        throw xsflib::FormatError("Line " + std::to_string(line) + ": " + error.what());
    }
}

// Refuses line `line`, a filler run or the filler line, in `native` for a driver that can't put songs into filler runs.
void FailWithoutFillerRuns(const NativeArchive& native, std::size_t line)
{
    const Driver* driver = native.driver ? DriverOf(native.engine) : nullptr;
    if (driver && !driver->Facts().filler_runs)
    {
        Fail(line, std::string(driver->Facts().title) + " can't put songs into filler runs");
    }
}

// The lines of a text, without their line breaks, and a reader of them.
class Lines
{
public:
    explicit Lines(std::string_view text)
    {
        std::size_t start = 0;
        while (start < text.size())
        {
            std::size_t end = text.find('\n', start);
            if (end == std::string_view::npos)
            {
                end = text.size();
            }

            std::string_view line = text.substr(start, end - start);
            if (line.ends_with('\r'))
            {
                line.remove_suffix(1);
            }

            lines_.push_back(line);
            start = end + 1;
        }
    }

    bool AtEnd() const
    {
        return next_ >= lines_.size();
    }

    // The next line and its number, from 1.
    std::pair<std::string_view, std::size_t> Next()
    {
        const std::size_t number = next_ + 1;
        return {lines_[next_++], number};
    }

    // `count` bytes in base64 on the lines that follow line `line`.
    std::vector<uint8_t> Bytes(uint64_t count, std::size_t line)
    {
        std::vector<uint8_t> bytes;
        while (bytes.size() < count)
        {
            if (AtEnd())
            {
                Fail(line, "the text ends before the " + std::to_string(count) + " bytes");
            }

            const auto [text, number] = Next();
            const std::size_t first = text.find_first_not_of(" \t");
            const std::size_t last = text.find_last_not_of(" \t");
            DecodeBase64(first == std::string_view::npos ? std::string_view() : text.substr(first, last - first + 1),
                         bytes, number);
        }

        if (bytes.size() != count)
        {
            Fail(line, "the base64 contains more than " + std::to_string(count) + " bytes");
        }

        return bytes;
    }

private:
    std::vector<std::string_view> lines_;
    std::size_t next_ = 0;
};

// The engine's, the sequences' and the native archive's lines, then each chunk.
enum class Section
{
    kHeader,
    kSequence,
    kNative,
    kChunks,
};

// `text` without the UTF-8 byte-order mark that some editors on Windows put at the start of a file.
std::string_view WithoutByteOrderMark(std::string_view text)
{
    constexpr std::string_view kMark = "\xEF\xBB\xBF";
    if (text.starts_with(kMark))
    {
        text.remove_prefix(kMark.size());
    }

    return text;
}

// A read of a file in text: the parts that its lines give so far, and the lines that gave them. ReadLine passes each
// line to the member that reads its word.
class TextReader
{
public:
    explicit TextReader(std::string_view text);

    // Reads every line.
    void ReadLines();

    // Checks the parts once every line is read, and makes the file of them.
    File Finish();

private:
    // Adds the sequence that the lines are reading to `sequences_`.
    void FinishDraft();

    // Reads one line, `tokens`, of line number `number`.
    void ReadLine(const std::vector<Token>& tokens, std::size_t number);

    // Each reads a line of its kind and returns true, or returns false for a line of another kind. The kinds are the
    // settings before the sequences, the starts of a sequence and of the native archive, and the lines of the native
    // archive.
    bool ReadSettingLine(const std::vector<Token>& tokens, std::size_t number);
    bool ReadSequenceLine(const std::vector<Token>& tokens, std::size_t number);
    bool ReadNativeLine(const std::vector<Token>& tokens, std::size_t number);

    // Reads the engine or the mixing of the file.
    void ReadEngineLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the mixing of a sound.
    void ReadSoundMixingLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the engine of a sound.
    void ReadSoundEngineLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads a tag of the file or of a sound.
    void ReadTagLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the length of a sound.
    void ReadSoundLengthLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the colour of a track.
    void ReadTrackColourLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads a line of an instrument map.
    void ReadInstrumentMapLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads a line of a bank map.
    void ReadBankMapLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the line that starts a sequence.
    void ReadSequenceStartLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the line that starts the native archive.
    void ReadNativeStartLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the build of the native archive.
    void ReadBuildLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the filler line of the native archive.
    void ReadFillerLine(std::size_t number);
    // Reads the note arguments of the native archive.
    void ReadNoteArgumentsLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads a chunk and the bytes that follow its line.
    void ReadChunkLine(const std::vector<Token>& tokens, std::size_t number);
    // Reads the BANK or WAVE chunk `id`, of `bytes`, on line `number`.
    void ReadEditedModelChunk(const std::string& id, const std::vector<uint8_t>& bytes, std::size_t number);
    // Reads the GRPF chunk, of `bytes`, on line `number`.
    void ReadGroupFilesChunk(const std::vector<uint8_t>& bytes, std::size_t number);

    // Fails at line `number` with `message` once the sequences have begun.
    void RequireHeader(std::size_t number, const std::string& message) const;

    // Steps of Finish.
    void CheckHeaderAndView();
    void CheckTargets();
    void PlaceFields();
    std::optional<Model> ModelOfChunks();
    void CheckNoteKeys(const Driver* driver);
    void CheckCommandValues(const Driver* driver, const std::optional<Model>& model);

    Lines lines_;
    File file_;
    std::vector<Sequence> sequences_;
    std::optional<NativeArchive> native_;
    bool note_arguments_line_ = false;
    bool engine_ = false;
    bool header_ = false;
    bool group_files_ = false;
    bool streams_ = false;
    bool banks_ = false;
    bool waves_ = false;
    std::optional<std::vector<uint8_t>> sounds_chunk_;
    std::optional<std::vector<uint8_t>> items_chunk_;
    std::size_t model_line_ = 0;
    bool scene_open_ = false;
    std::array<bool, kAuxBuses> volume_given_{};
    EditorView view_;
    std::set<std::string> view_given_;
    std::size_t view_line_ = 0;
    TimelineLines timeline_lines_;
    Section section_ = Section::kHeader;
    std::optional<SequenceDraft> draft_;
    std::vector<std::size_t> choices_lines_;
    std::vector<std::vector<std::size_t>> item_lines_;
    std::vector<ntdlib::sar::Piece> pieces_;
    std::vector<FieldDraft> fields_;
    std::vector<std::map<std::string, uint32_t>> locals_;
};

TextReader::TextReader(std::string_view text) : lines_(WithoutByteOrderMark(text))
{
}

void TextReader::RequireHeader(std::size_t number, const std::string& message) const
{
    if (section_ != Section::kHeader)
    {
        Fail(number, message);
    }
}

void TextReader::ReadLines()
{
    while (!lines_.AtEnd())
    {
        const auto [line, number] = lines_.Next();
        const std::vector<Token> tokens = Tokenize(line, number);
        if (tokens.empty())
        {
            continue;
        }

        ReadLine(tokens, number);
    }
}

void TextReader::FinishDraft()
{
    if (draft_)
    {
        FinishSequence(*draft_);
        sequences_.push_back(std::move(draft_->sequence));
        locals_.push_back(std::move(draft_->locals));
        choices_lines_.push_back(draft_->choices_line);
        item_lines_.push_back(std::move(draft_->lines));
        draft_.reset();
    }
}

void TextReader::ReadLine(const std::vector<Token>& tokens, std::size_t number)
{
    // The first line names the format and its version.
    if (!header_)
    {
        const std::optional<uint64_t> major = tokens.size() == 2 && tokens[0].Is("hok")
                                                  ? Integer(tokens[1].text.substr(0, tokens[1].text.find('.')))
                                                  : std::nullopt;
        if (!major)
        {
            Fail(number, "a Hokuto file in text starts with hok and its version, such as hok 1.0");
        }

        if (*major != kMajorVersion)
        {
            Fail(number, "a later version of Hokuto saved the file, in version " + tokens[1].text +
                             " of the format. This version reads only version " + std::to_string(kMajorVersion));
        }

        header_ = true;
        return;
    }

    // A label, in a sequence.
    if (tokens.size() == 2 && tokens[1].Is(":"))
    {
        if (!draft_)
        {
            Fail(number, "a label has to be in a sequence");
        }

        const auto item = static_cast<uint32_t>(draft_->sequence.items.size());
        const bool local = !tokens[0].quoted && tokens[0].text.starts_with(".");
        std::map<std::string, uint32_t>& names = local ? draft_->locals : draft_->labels;
        if (!names.emplace(tokens[0].text, item).second)
        {
            Fail(number, "the sequence already has a label named " + tokens[0].text);
        }

        return;
    }

    // A scene's values follow its line, before the sequences.
    const Token& word = tokens[0];
    if (section_ == Section::kHeader && (word.Is("variable") || word.Is("at")))
    {
        if (!scene_open_)
        {
            Fail(number, word.text + " belongs to a scene, after the scene's line");
        }

        ParseSceneValue(tokens, number, file_.scenes.back());
        return;
    }

    scene_open_ = false;
    if (ReadSettingLine(tokens, number) || ReadSequenceLine(tokens, number) || ReadNativeLine(tokens, number))
    {
        return;
    }

    if (word.Is("chunk") && tokens.size() == 3)
    {
        ReadChunkLine(tokens, number);
    }
    else if (draft_)
    {
        const auto at = static_cast<uint32_t>(draft_->sequence.items.size());
        draft_->sequence.items.push_back(ParseCommand(tokens, number, at, draft_->references));
        draft_->lines.push_back(number);
        if (draft_->choices_line == 0 && HasChoices(draft_->sequence.items.back()))
        {
            draft_->choices_line = number;
        }
    }
    else
    {
        Fail(number, word.text + " isn't valid here");
    }
}

bool TextReader::ReadSettingLine(const std::vector<Token>& tokens, std::size_t number)
{
    const Token& word = tokens[0];
    if (word.Is("engine") || word.Is("mixing"))
    {
        ReadEngineLine(tokens, number);
    }
    else if (word.Is("sound_mixing"))
    {
        ReadSoundMixingLine(tokens, number);
    }
    else if (word.Is("sound_engine"))
    {
        ReadSoundEngineLine(tokens, number);
    }
    else if (word.Is("effect"))
    {
        RequireHeader(number, "the effects come before the sequences");

        auto [bus, effect] = ParseEffect(tokens, number);
        file_.aux_effects[bus].push_back(std::move(effect));
    }
    else if (word.Is("bus_volume"))
    {
        RequireHeader(number, "the buses' volumes come before the sequences");

        const auto [bus, volume] = ParseBusVolume(tokens, number);
        if (std::exchange(volume_given_[bus], true))
        {
            Fail(number, "the file sets the volume of bus " + tokens[1].text + " twice");
        }

        file_.aux_volumes[bus] = volume;
    }
    else if (word.Is("tag") || word.Is("sound_tag"))
    {
        ReadTagLine(tokens, number);
    }
    else if (word.Is("sound_length"))
    {
        ReadSoundLengthLine(tokens, number);
    }
    else if (word.Is("scene"))
    {
        RequireHeader(number, "the scenes come before the sequences");
        if (tokens.size() != 3)
        {
            Fail(number, "a scene's line is scene, its sound's number and its name");
        }

        Scene& scene = file_.scenes.emplace_back();
        scene.sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
        scene.name = tokens[2].text;
        scene_open_ = true;
    }
    else if (word.Is("track_colour"))
    {
        ReadTrackColourLine(tokens, number);
    }
    else if (word.Is("instrument_map"))
    {
        ReadInstrumentMapLine(tokens, number);
    }
    else if (word.Is("bank_map"))
    {
        ReadBankMapLine(tokens, number);
    }
    else if (word.Is("timeline"))
    {
        RequireHeader(number, "the timelines come before the sequences");

        ParseTimeline(tokens, number, file_, timeline_lines_);
    }
    else if (word.Is("view"))
    {
        RequireHeader(number, "the view comes before the sequences");

        ParseView(tokens, number, view_, view_given_);
        view_line_ = view_line_ == 0 ? number : view_line_;
    }
    else
    {
        return false;
    }

    return true;
}

bool TextReader::ReadSequenceLine(const std::vector<Token>& tokens, std::size_t number)
{
    const Token& word = tokens[0];
    if (word.Is("sequence") && tokens.size() == 2)
    {
        ReadSequenceStartLine(tokens, number);
    }
    else if (word.Is("labels") && draft_)
    {
        if (!draft_->order.empty())
        {
            Fail(number, "a sequence has one labels line");
        }

        draft_->order_line = number;
        for (std::size_t t = 1; t < tokens.size(); ++t)
        {
            draft_->order.push_back(tokens[t].text);
        }
    }
    else if (word.Is("native") && tokens.size() == 2)
    {
        ReadNativeStartLine(tokens, number);
    }
    else
    {
        return false;
    }

    return true;
}

bool TextReader::ReadNativeLine(const std::vector<Token>& tokens, std::size_t number)
{
    const Token& word = tokens[0];
    if (word.Is("bytes") && section_ == Section::kNative && tokens.size() == 2)
    {
        const uint64_t size = ParseInteger(tokens[1], UINT64_MAX >> 1, number, "a size");
        pieces_.emplace_back(lines_.Bytes(size, number));
    }
    else if (word.Is("revision") && section_ == Section::kNative && tokens.size() == 2 && native_ && native_->driver)
    {
        const uint64_t most = DriverOf(native_->engine)->Facts().most_revision;
        native_->driver->revision =
            static_cast<uint8_t>(ParseInteger(tokens[1], most, number, "a revision of the driver"));
    }
    else if (word.Is("free_space") && section_ == Section::kNative && tokens.size() == 4 && tokens[2].Is("padding") &&
             native_ && native_->driver)
    {
        native_->driver->free_start = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a place"));
        native_->driver->padding = static_cast<uint8_t>(ParseInteger(tokens[3], 0xFF, number, "a padding byte"));
    }
    else if (word.Is("filler_run") && section_ == Section::kNative && tokens.size() == 3 && native_ && native_->driver)
    {
        FailWithoutFillerRuns(*native_, number);
        native_->driver->filler.push_back(
            {.start = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a place")),
             .size = static_cast<uint32_t>(ParseInteger(tokens[2], 0xFFFFFFFF, number, "a size"))});
    }
    else if (word.Is("list_room") && section_ == Section::kNative && tokens.size() == 5 && tokens[3].Is("fill") &&
             native_ && native_->driver && DriverOf(native_->engine)->Facts().rooms)
    {
        native_->driver->rooms.push_back(
            {.piece = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a piece")),
             .end = static_cast<uint32_t>(ParseInteger(tokens[2], 0xFFFFFFFF, number, "a place")),
             .fill = static_cast<uint8_t>(ParseInteger(tokens[4], 0xFF, number, "a fill byte"))});
    }
    else if (word.Is("sequence_file") && section_ == Section::kNative && tokens.size() == 4 &&
             tokens[2].Is(native_ && (native_->engine == Engine::kGba || native_->driver) ? "room" : "version"))
    {
        const bool room = native_->engine == Engine::kGba || native_->driver.has_value();
        ntdlib::sar::SequencePiece piece;
        piece.sequence = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sequence"));
        (room ? piece.room : piece.version) =
            static_cast<uint32_t>(ParseInteger(tokens[3], 0xFFFFFFFF, number, room ? "a room" : "a version"));
        if (piece.sequence >= sequences_.size())
        {
            Fail(number, "the file has no sequence " + tokens[1].text);
        }

        pieces_.emplace_back(piece);
    }
    else if (word.Is("padding") && section_ == Section::kNative && tokens.size() == 2)
    {
        const auto alignment = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "an alignment"));
        if (alignment == 0)
        {
            Fail(number, "padding aligns to a multiple of at least 1 byte");
        }

        pieces_.emplace_back(ntdlib::sar::Padding{alignment});
    }
    else if (word.Is("field") && section_ == Section::kNative)
    {
        fields_.push_back(ParseField(tokens, number));
    }
    else if (word.Is("tables") && section_ == Section::kNative && tokens.size() == 1)
    {
        if (native_->tables)
        {
            Fail(number, "the native archive has one tables line");
        }

        native_->tables.emplace();
    }
    else if (IsTablesWord(word) && section_ == Section::kNative)
    {
        if (!native_->tables)
        {
            Fail(number, word.text + " belongs to the tables, after the tables line");
        }

        ParseTablesLine(tokens, number, *native_->tables, native_->game_sizes);
    }
    else if (word.Is("build") && section_ == Section::kNative &&
             (tokens.size() == 2 || (tokens.size() == 3 && tokens[2].Is("october_2006"))))
    {
        ReadBuildLine(tokens, number);
    }
    else if (word.Is("filler") && section_ == Section::kNative && tokens.size() == 1)
    {
        ReadFillerLine(number);
    }
    else if (word.Is("note_arguments") && section_ == Section::kNative && tokens.size() == 2)
    {
        ReadNoteArgumentsLine(tokens, number);
    }
    else if (word.Is("game_size") && section_ == Section::kNative && tokens.size() == 2)
    {
        if (native_->game_sizes)
        {
            Fail(number, "the native archive has one game_size line");
        }

        if (native_->tables)
        {
            Fail(number, "the native archive's game_size comes before the tables");
        }

        native_->game_sizes.emplace().archive = ParseInteger(tokens[1], UINT64_MAX, number, "a size");
    }
    else
    {
        return false;
    }

    return true;
}

void TextReader::ReadEngineLine(const std::vector<Token>& tokens, std::size_t number)
{
    const Token& word = tokens[0];
    if (section_ != Section::kHeader || tokens.size() != 2)
    {
        Fail(number, word.text + " comes once, before the sequences, with a name");
    }

    if (word.Is("engine"))
    {
        const std::optional<Engine> named = EngineNamed(tokens[1].text);
        if (!named)
        {
            Fail(number, "there's no engine named " + tokens[1].text);
        }

        file_.engine = *named;
        engine_ = true;
    }
    else
    {
        const std::optional<Mixing> named = MixingNamed(tokens[1].text);
        if (!named)
        {
            Fail(number, "there's no mixing mode named " + tokens[1].text);
        }

        file_.mixing = *named;
    }
}

void TextReader::ReadSoundMixingLine(const std::vector<Token>& tokens, std::size_t number)
{
    if (section_ != Section::kHeader || tokens.size() != 3)
    {
        Fail(number,
             "a sound's mixing mode is sound_mixing, the sound's number and the mode, before the "
             "sequences");
    }

    const auto sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
    const std::optional<Mixing> named = MixingNamed(tokens[2].text);
    if (!named)
    {
        Fail(number, "there's no mixing mode named " + tokens[2].text);
    }

    if (!file_.sound_mixing.emplace(sound, *named).second)
    {
        Fail(number, "sound " + tokens[1].text + "'s mixing mode comes twice");
    }
}

void TextReader::ReadSoundEngineLine(const std::vector<Token>& tokens, std::size_t number)
{
    if (section_ != Section::kHeader || tokens.size() != 3)
    {
        Fail(number, "a sound's engine is sound_engine, the sound's number and the engine, before the sequences");
    }

    const auto sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
    const std::optional<Engine> named = EngineNamed(tokens[2].text);
    if (!named)
    {
        Fail(number, "there's no engine named " + tokens[2].text);
    }

    if (!FactsOf(*named).hosts_other_consoles)
    {
        Fail(number, "a sound of another console can't play on the " + EngineWords(*named) + " engine");
    }

    if (!file_.sound_engines.emplace(sound, *named).second)
    {
        Fail(number, "sound " + tokens[1].text + "'s engine comes twice");
    }
}

void TextReader::ReadTagLine(const std::vector<Token>& tokens, std::size_t number)
{
    const Token& word = tokens[0];
    RequireHeader(number, "the tags come before the sequences");

    // The file's tag, or a sound's.
    const bool of_sound = word.Is("sound_tag");
    const std::size_t at = of_sound ? 2 : 1;
    if (tokens.size() != at + 2 || tokens[at].quoted)
    {
        Fail(number, of_sound ? "a sound's tag is sound_tag, the sound's number, the tag's key and its value"
                              : "a tag is tag, its key and its value");
    }

    std::string whose;
    Tags* tags = &file_.tags;
    if (of_sound)
    {
        const auto sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
        whose = "sound " + std::to_string(sound) + "'s ";
        tags = &file_.sound_tags[sound];
    }

    const std::string& key = tokens[at].text;
    const std::string& value = tokens[at + 1].text;
    if (const std::optional<std::string> problem = TagProblem(key, value))
    {
        Fail(number, whose + *problem);
    }

    if (!tags->emplace(key, value).second)
    {
        Fail(number, whose + "tag " + key + " comes twice");
    }
}

void TextReader::ReadSoundLengthLine(const std::vector<Token>& tokens, std::size_t number)
{
    RequireHeader(number, "the sounds' lengths come before the sequences");

    // The sound, its length, its fade and its loops, then the loop's ticks and silent where they come.
    std::size_t count = tokens.size();
    SoundLength length;
    if (count > 5 && tokens[count - 1].Is("silent"))
    {
        length.silent = true;
        count--;
    }
    if (count != 5 && count != 7)
    {
        Fail(number,
             "a sound's lengths are sound_length, the sound's number, its length, its fade and its loops, "
             "then the ticks where its loop starts and ends, and silent for a sound that never makes a sound");
    }

    const auto whole = [&tokens, number](std::size_t index, uint64_t max, const char* what)
    {
        return static_cast<uint32_t>(ParseInteger(tokens[index], max, number, what));
    };
    const uint32_t sound = whole(1, 0xFFFFFFFF, "a sound's number");
    length.length = whole(2, 0xFFFFFFFF, "a sound's length");
    length.fade = whole(3, 0xFFFFFFFF, "a sound's fade");
    length.loops = static_cast<uint8_t>(whole(4, 0xFF, "a sound's number of loops"));
    if (count == 7)
    {
        length.loop = LoopTicks{whole(5, 0xFFFFFFFF, "a loop's start"), whole(6, 0xFFFFFFFF, "a loop's end")};
    }

    const std::string whose = "sound " + std::to_string(sound) + "'s lengths";
    if (const std::optional<std::string> problem = LengthProblem(length))
    {
        Fail(number, whose + " " + *problem);
    }

    if (!file_.lengths.emplace(sound, length).second)
    {
        Fail(number, whose + " come twice");
    }
}

void TextReader::ReadTrackColourLine(const std::vector<Token>& tokens, std::size_t number)
{
    RequireHeader(number, "the tracks' colours come before the sequences");
    if (tokens.size() != 6)
    {
        Fail(number,
             "a track's colour is track_colour, the sound's number, the track's number, and the "
             "colour's red, green and blue");
    }

    TrackPlace place;
    place.sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
    place.track = static_cast<uint8_t>(ParseInteger(tokens[2], 0xFF, number, "a track's number"));
    Colour colour;
    colour.red = static_cast<uint8_t>(ParseInteger(tokens[3], 0xFF, number, "a colour's red"));
    colour.green = static_cast<uint8_t>(ParseInteger(tokens[4], 0xFF, number, "a colour's green"));
    colour.blue = static_cast<uint8_t>(ParseInteger(tokens[5], 0xFF, number, "a colour's blue"));
    if (!file_.track_colours.emplace(place, colour).second)
    {
        Fail(number, "track " + std::to_string(place.track) + " of sound " + std::to_string(place.sound) +
                         " already has a colour");
    }
}

void TextReader::ReadInstrumentMapLine(const std::vector<Token>& tokens, std::size_t number)
{
    RequireHeader(number, "the instrument maps come before the sequences");
    if (tokens.size() != 6 || !(tokens[2].Is("program") || tokens[2].Is("drums")) || !tokens[4].Is("voice"))
    {
        Fail(number,
             "a pairing of an instrument map is instrument_map, the voice group's address, program or drums "
             "and the program's number, and voice and the voice's number");
    }

    const auto voices = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a voice group's address"));
    const MidiInstrument instrument{tokens[2].Is("drums"),
                                    static_cast<int>(ParseInteger(tokens[3], 127, number, "a program"))};
    const auto voice = static_cast<int>(ParseInteger(tokens[5], 127, number, "a voice"));
    if (!file_.instrument_maps[voices].emplace(instrument, voice).second)
    {
        Fail(number,
             "the instrument map of " + tokens[1].text + " already pairs " + tokens[2].text + " " + tokens[3].text);
    }
}

void TextReader::ReadBankMapLine(const std::vector<Token>& tokens, std::size_t number)
{
    RequireHeader(number, "the instrument maps come before the sequences");
    const bool drum_note = tokens.size() == 8 && tokens[2].Is("drum_note") && tokens[6].Is("key");
    if (!(drum_note || (tokens.size() == 6 && tokens[2].Is("program"))) || !tokens[4].Is("instrument"))
    {
        Fail(number,
             "a pairing of a bank's instrument map is bank_map, the bank's index, program and the MIDI "
             "file's program, and instrument and the bank's program, or drum_note and the note, instrument "
             "and the bank's program, and key and the key");
    }

    const auto bank = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a bank's index"));
    const auto source = static_cast<int>(ParseInteger(tokens[3], 127, number, drum_note ? "a note" : "a program"));
    const auto target = static_cast<int>(ParseInteger(tokens[5], 0xFFFF, number, "a program of a bank"));
    BankMidiMap& map = file_.bank_maps[bank];
    bool added = false;
    if (drum_note)
    {
        const auto key = static_cast<int>(ParseInteger(tokens[7], 127, number, "a key"));
        added = map.drum_notes.emplace(source, MidiDrumNote{target, key}).second;
    }
    else
    {
        added = map.programs.emplace(source, target).second;
    }
    if (!added)
    {
        Fail(number, "the instrument map of bank " + tokens[1].text + " already pairs " + tokens[2].text + " " +
                         tokens[3].text);
    }
}

void TextReader::ReadSequenceStartLine(const std::vector<Token>& tokens, std::size_t number)
{
    if (section_ > Section::kSequence)
    {
        Fail(number, "the sequences come before the native archive and the chunks");
    }

    FinishDraft();
    const uint64_t index = ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sequence's number");
    if (index != sequences_.size())
    {
        Fail(number,
             "the sequences are numbered from 0 in order. This one has to be " + std::to_string(sequences_.size()));
    }

    section_ = Section::kSequence;
    draft_.emplace();
}

void TextReader::ReadNativeStartLine(const std::vector<Token>& tokens, std::size_t number)
{
    if (section_ > Section::kSequence)
    {
        Fail(number, "the native archive comes once, before the chunks");
    }

    FinishDraft();
    const std::optional<Engine> engine = EngineNamed(tokens[1].text);
    if (!engine)
    {
        Fail(number, "there's no native archive type named " + tokens[1].text);
    }

    section_ = Section::kNative;
    native_ = NativeArchive{};
    SetEngine(*native_, *engine);
}

void TextReader::ReadBuildLine(const std::vector<Token>& tokens, std::size_t number)
{
    if (native_->engine != Engine::kWii)
    {
        Fail(number, "only a native archive of the Wii has a build of the Wii's sound library");
    }

    if (native_->wii_build)
    {
        Fail(number, "the native archive has one build line");
    }

    if (!pieces_.empty())
    {
        Fail(number, "the native archive's build comes before its pieces");
    }

    ntdlib::SndBuild build;
    build.date = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a date"));
    build.october_2006 = tokens.size() == 3;
    native_->wii_build = build;
}

void TextReader::ReadFillerLine(std::size_t number)
{
    if (native_->engine != Engine::kGba && !native_->driver)
    {
        Fail(number, "only a GBA ROM's songs can go into filler runs");
    }

    FailWithoutFillerRuns(*native_, number);
    if (native_->gba_filler)
    {
        Fail(number, "the native archive has one filler line");
    }

    if (!pieces_.empty())
    {
        Fail(number, "the native archive's filler line comes before its pieces");
    }

    native_->gba_filler = true;
}

void TextReader::ReadNoteArgumentsLine(const std::vector<Token>& tokens, std::size_t number)
{
    if (native_->engine != Engine::kGba)
    {
        Fail(number, "only a GBA ROM with MusicPlayer2000 gives its notes' argument bytes");
    }

    if (note_arguments_line_)
    {
        Fail(number, "the native archive has one note_arguments line");
    }

    if (!pieces_.empty())
    {
        Fail(number, "the native archive's note_arguments line comes before its pieces");
    }

    native_->gba_note_arguments = static_cast<int>(ParseInteger(tokens[1], 3, number, "a count of bytes"));
    if (native_->gba_note_arguments < 2)
    {
        Fail(number, "a note of MusicPlayer2000 reads 2 or 3 argument bytes");
    }

    note_arguments_line_ = true;
}

void TextReader::ReadChunkLine(const std::vector<Token>& tokens, std::size_t number)
{
    FinishDraft();
    section_ = Section::kChunks;
    const uint64_t size = ParseInteger(tokens[2], UINT64_MAX >> 1, number, "a size");
    if (tokens[1].text.size() != 4)
    {
        Fail(number, "a chunk's ID has four characters");
    }

    std::vector<uint8_t> bytes = lines_.Bytes(size, number);
    if (tokens[1].text == kSoundsChunk || tokens[1].text == kItemsChunk)
    {
        // The model's chunks hold the archive of a file without a native archive.
        std::optional<std::vector<uint8_t>>& chunk = tokens[1].text == kSoundsChunk ? sounds_chunk_ : items_chunk_;
        if (native_ || chunk)
        {
            Fail(number, "the " + tokens[1].text + " chunk comes once, in a file without a native archive");
        }

        chunk = std::move(bytes);
        model_line_ = model_line_ == 0 ? number : model_line_;
        return;
    }

    if (tokens[1].text == kStreamsChunk)
    {
        // The streams belong to the native archive, after its group files.
        if (!native_ || streams_)
        {
            Fail(number, "the STRM chunk comes once, after the native archive");
        }

        AtLine(number, [&] { native_->streams = ReadStreams(bytes); });

        streams_ = true;
        return;
    }

    if (tokens[1].text == kCartridgeChunk)
    {
        // The cartridge image belongs to a native archive of the DS, after its streams.
        if (!native_ || (native_->engine != Engine::kDs && native_->engine != Engine::kDse) || native_->cartridge)
        {
            Fail(number, "the CART chunk comes once, after a native archive of the DS");
        }

        AtLine(number, [&] { native_->cartridge = ReadCartridge(bytes); });

        return;
    }

    if (tokens[1].text == kBanksChunk || tokens[1].text == kWavesChunk)
    {
        ReadEditedModelChunk(tokens[1].text, bytes, number);
        return;
    }

    if (tokens[1].text != kGroupFilesChunk)
    {
        if (IsHokutoChunk(tokens[1].text))
        {
            Fail(number, "a chunk line holds only a chunk that Hokuto doesn't read, not " + tokens[1].text);
        }

        file_.other_chunks.push_back({tokens[1].text, std::move(bytes)});
        return;
    }

    ReadGroupFilesChunk(bytes, number);
}

void TextReader::ReadEditedModelChunk(const std::string& id, const std::vector<uint8_t>& bytes, std::size_t number)
{
    // The models of the edited banks and waves come once each, after the archive.
    bool& read = id == kBanksChunk ? banks_ : waves_;
    if ((!native_ && !items_chunk_) || read)
    {
        Fail(number, "the " + id + " chunk comes once, after the archive");
    }

    const auto read_chunk = [&]
    {
        if (id == kBanksChunk)
        {
            file_.banks = ReadBanks(bytes);
        }
        else
        {
            file_.waves = ReadWaves(bytes);
        }
    };
    AtLine(number, read_chunk);

    read = true;
}

void TextReader::ReadGroupFilesChunk(const std::vector<uint8_t>& bytes, std::size_t number)
{
    // The group files belong to the native archive.
    if (!native_ || group_files_ || streams_)
    {
        Fail(number, "the GRPF chunk comes once, after the native archive and before its streams");
    }

    try
    {
        native_->group_files = ReadGroupFiles(bytes, sequences_);
    }
    catch (const binary::EndsEarly&)
    {
        Fail(number, "the GRPF chunk ends early");
    }
    catch (const xsflib::FormatError& error)
    {
        throw xsflib::FormatError("Line " + std::to_string(number) + ": " + error.what());
    }

    group_files_ = true;
}

void TextReader::CheckHeaderAndView()
{
    if (!header_)
    {
        Fail(1, "the text is empty");
    }

    if (!engine_)
    {
        Fail(1, "the file names no engine");
    }

    // A view gives each part that a writer always writes, and holds what the program can show.
    if (view_line_ != 0)
    {
        for (const std::string_view part : {"sound", "position", "cycle", "zoom", "scroll", "key_zoom", "key_scroll"})
        {
            if (!view_given_.contains(std::string(part)))
            {
                Fail(view_line_, "the view has no " + std::string(part));
            }
        }

        if (const std::optional<std::string> problem = ViewProblem(view_))
        {
            Fail(view_line_, "the view " + *problem);
        }

        // A view without a window in front puts the List Editor in front. Hokuto opened such a file that way.
        if (!view_given_.contains("front"))
        {
            view_.front = ViewFront::kListEditor;
        }

        file_.view = view_;
    }
}

void TextReader::CheckTargets()
{
    // The targets lie within their sequences.
    for (std::size_t s = 0; s < sequences_.size(); ++s)
    {
        const std::vector<Command>& items = sequences_[s].items;
        for (const Command& item : items)
        {
            if (!item.target_outside && item.target > items.size())
            {
                Fail(1, "a command of sequence " + std::to_string(s) + " goes past its end");
            }
        }
    }
}

void TextReader::PlaceFields()
{
    // The native archive takes its pieces.
    if (native_)
    {
        native_->pieces = std::move(pieces_);
    }

    // The fields' places at the archive's end follow the last piece, and labels give items their numbers.
    for (FieldDraft& field_draft : fields_)
    {
        ntdlib::sar::Field& field = field_draft.field;
        for (ntdlib::sar::Place* place : {&field.at, &field.from, &field.to})
        {
            place->piece = place->piece == kEndPiece ? static_cast<uint32_t>(native_->pieces->size()) : place->piece;
        }

        if (field_draft.from_label)
        {
            field.from.offset =
                ItemOfLabel(*native_, sequences_, locals_, field.from, *field_draft.from_label, field_draft.line);
        }

        if (field_draft.to_label)
        {
            field.to.offset =
                ItemOfLabel(*native_, sequences_, locals_, field.to, *field_draft.to_label, field_draft.line);
        }

        if (const std::optional<std::string> problem = FieldProblem(*native_, sequences_, field))
        {
            Fail(field_draft.line, "the field " + *problem);
        }

        if (field.bytes == 2 && native_->engine != Engine::kDse &&
            !(native_->driver && DriverOf(native_->engine)->Facts().two_byte_fields))
        {
            Fail(field_draft.line, "only DSE files and ROMs with Konami's driver have fields of 2 bytes");
        }

        native_->fields.push_back(field);
    }
}

std::optional<Model> TextReader::ModelOfChunks()
{
    // The model of a file in the model form.
    std::optional<Model> model;
    if (sounds_chunk_ || items_chunk_)
    {
        if (!sounds_chunk_ || !items_chunk_)
        {
            Fail(model_line_, "a file in the model form has to have both the SNDS and the ITMS chunk");
        }

        AtLine(model_line_, [&] { model = ReadModel(*sounds_chunk_, *items_chunk_); });
    }

    return model;
}

void TextReader::CheckNoteKeys(const Driver* driver)
{
    // A note's key is from 0 to 127, except on the drivers where it stores the byte of the command.
    if (!driver || !driver->Facts().key_holds_byte)
    {
        for (std::size_t s = 0; s < sequences_.size(); ++s)
        {
            for (std::size_t i = 0; i < sequences_[s].items.size(); ++i)
            {
                if (sequences_[s].items[i].kind == Kind::kNote && sequences_[s].items[i].key > 127)
                {
                    Fail(item_lines_[s][i], "a note's key has to be a whole number from 0 to 127, not " +
                                                std::to_string(sequences_[s].items[i].key));
                }
            }
        }
    }
}

void TextReader::CheckCommandValues(const Driver* driver, const std::optional<Model>& model)
{
    // The values of each command have to fit the bytes of the console: those of MP2K in a GBA ROM, those of the DS in a
    // sound archive of the DS, those of DSE in a set of DSE files, and those of the driver revision in a ROM of another
    // GBA driver. Every other archive and model follows the rules of its console (CommandProblem).
    if (driver)
    {
        const std::vector<ntdlib::sar::Piece>& native_pieces = *native_->pieces;
        const std::vector<std::vector<uint32_t>> entries =
            driver->TrackEntries(native_pieces, native_->fields, native_->driver->revision);
        for (std::size_t p = 0; p < native_pieces.size(); ++p)
        {
            const auto* piece = std::get_if<ntdlib::sar::SequencePiece>(&native_pieces[p]);
            if (!piece)
            {
                continue;
            }

            const std::size_t s = piece->sequence;
            const auto problem = DriverProblem(*driver, sequences_[s], entries[p], native_->driver->revision);
            if (problem && !item_lines_[s].empty())
            {
                Fail(item_lines_[s][std::min(problem->first, item_lines_[s].size() - 1)], problem->second);
            }
        }
    }
    else if (native_ &&
             (native_->engine == Engine::kGba || native_->engine == Engine::kDs || native_->engine == Engine::kDse))
    {
        for (std::size_t s = 0; s < sequences_.size(); ++s)
        {
            const auto problem = native_->engine == Engine::kGba
                                     ? Mp2kProblem(sequences_[s], native_->gba_note_arguments)
                                 : native_->engine == Engine::kDse ? DseProblem(sequences_[s])
                                                                   : NtrProblem(sequences_[s]);
            if (problem && !item_lines_[s].empty())
            {
                Fail(item_lines_[s][std::min(problem->first, item_lines_[s].size() - 1)], problem->second);
            }
        }
    }
    else if (native_ || model)
    {
        const ntdlib::nwsnd::Grammar grammar = native_ ? GrammarOf(*native_) : GrammarOf(*model);
        for (std::size_t s = 0; s < sequences_.size(); ++s)
        {
            for (std::size_t i = 0; i < sequences_[s].items.size(); ++i)
            {
                if (const std::optional<std::string> problem = CommandProblem(grammar, sequences_[s].items[i]))
                {
                    Fail(item_lines_[s][i], *problem);
                }
            }
        }
    }
}

File TextReader::Finish()
{
    FinishDraft();
    CheckHeaderAndView();
    if (native_)
    {
        CheckNativeEngine(file_.engine, *native_);
    }

    CheckTargets();
    PlaceFields();
    std::optional<Model> model = ModelOfChunks();
    const Driver* driver = native_ && native_->driver ? DriverOf(native_->engine) : nullptr;
    CheckNoteKeys(driver);
    CheckCommandValues(driver, model);

    // The file shares its parts once they're whole. The model builds the native archive from the sequences.
    file_.sequences = ShareSequences(std::move(sequences_));
    if (native_)
    {
        file_.native = std::move(*native_);
    }

    if (model)
    {
        AtLine(model_line_, [&] { file_ = WithModel(std::move(file_), *model); });
    }

    // Each timeline names a sequence of the file.
    for (const auto& [sequence, line] : timeline_lines_.bars)
    {
        if (sequence >= file_.sequences.size())
        {
            Fail(line, "the file has no sequence " + std::to_string(sequence));
        }
    }

    if (const std::optional<std::string> problem = TimelinesProblem(file_))
    {
        Fail(timeline_lines_.bars.empty() ? timeline_lines_.none : timeline_lines_.bars.begin()->second,
             "the timelines " + *problem);
    }

    // The encoder's choices belong to the sequence file of their sequence in the native archive or its group files.
    if (const std::optional<std::size_t> sequence = UnfiledChoices(file_))
    {
        const std::string needed =
            "a sequence_file of sequence " + std::to_string(*sequence) + " in the native archive or its group files";
        Fail(choices_lines_[*sequence],
             "[value_bytes], [unused_time], [unused_random], [unused_variable], [repeated], [arguments] and "
             "[length_code] require " +
                 needed);
    }

    if (const std::optional<std::size_t> sequence = ForeignChoices(file_))
    {
        Fail(choices_lines_[*sequence], file_.native->engine == Engine::kGba
                                            ? "a GBA ROM holds only [repeated], [arguments] and [length_code]"
                                            : "[repeated], [arguments] and [length_code] belong to a GBA ROM");
    }

    return std::move(file_);
}

} // namespace

File ReadText(std::string_view text)
{
    TextReader reader(text);
    reader.ReadLines();
    return reader.Finish();
}

bool IsText(std::span<const uint8_t> bytes)
{
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return WithoutByteOrderMark(text).starts_with("hok ");
}

File ReadEitherForm(std::span<const uint8_t> bytes)
{
    if (IsBinary(bytes))
    {
        return Read(bytes);
    }
    if (IsText(bytes))
    {
        return ReadText(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }

    throw xsflib::FormatError(NotKindText(bytes, "a Hokuto file"));
}

} // namespace hokuto::format
