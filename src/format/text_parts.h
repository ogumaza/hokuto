// SPDX-License-Identifier: MIT

// The parts of the text form that its files share: the writer of lines, the tokens of a line, the drafts that the
// reader fills, and the steps that WriteText and ReadText call. Only the text files of src/format include it.

#pragma once

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ntdlib/agb/rom.h"
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

#include "format/codes.h"
#include "format/command_values.h"
#include "format/drivers.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"

namespace hokuto::format::text_parts
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

constexpr std::size_t kBase64Bytes = 57; // the bytes of a line of base64: 76 characters

// A bank slot that holds no bank.
constexpr uint32_t kNoBank = 0xFFFFFFFF;

// The values of a sound that take a byte, by name, in the order of a sound's line.
constexpr std::array<std::pair<std::string_view, uint8_t NativeSound::*>, 6> kSoundBytes = {{
    {"volume", &NativeSound::volume},
    {"pan_mode", &NativeSound::pan_mode},
    {"pan_curve", &NativeSound::pan_curve},
    {"player_priority", &NativeSound::player_priority},
    {"actor_player", &NativeSound::actor_player},
    {"channel_priority", &NativeSound::channel_priority},
}};

std::string Base64(std::span<const uint8_t> bytes);

class TextOut
{
public:
    void Line(int indent, std::string_view text)
    {
        text_.append(static_cast<std::size_t>(indent) * 4, ' ');
        text_ += text;
        text_ += '\n';
    }

    // Bytes in base64, a line of kBase64Bytes at a time.
    void Bytes(int indent, std::span<const uint8_t> bytes)
    {
        for (std::size_t i = 0; i < bytes.size(); i += kBase64Bytes)
        {
            Line(indent, Base64(bytes.subspan(i, std::min(kBase64Bytes, bytes.size() - i))));
        }
    }

    std::string Take()
    {
        return std::move(text_);
    }

private:
    std::string text_;
};

struct Token
{
    bool Is(std::string_view word) const
    {
        return !quoted && text == word;
    }

    std::string text;
    bool quoted = false;
};

// A reference to a label that a command makes before the label may be known.
struct Reference
{
    std::size_t line = 0;
    uint32_t item = 0;
    Token name;
};

// A sequence while its lines are read: the line of each item, its labels and local labels by name, its references to
// them, and the first line with the encoder's choices.
struct SequenceDraft
{
    Sequence sequence;
    std::vector<std::size_t> lines; // the line of each item
    std::map<std::string, uint32_t> labels;
    std::map<std::string, uint32_t> locals;
    std::vector<Reference> references;
    std::vector<std::string> order;
    std::size_t order_line = 0;
    std::size_t choices_line = 0;
};

// The piece of a place at the archive's end, until the number of pieces is known.
constexpr uint32_t kEndPiece = 0xFFFFFFFF;

// A field's line, with the labels that name its places' items.
struct FieldDraft
{
    ntdlib::sar::Field field;
    std::optional<Token> from_label;
    std::optional<Token> to_label;
    std::size_t line = 0;
};

// The lines of the timelines that a reader has read: the line of each timeline's bars, by its sequence, and the line of
// none.
struct TimelineLines
{
    std::map<uint32_t, std::size_t> bars;
    std::size_t none = 0;
};

// Numbers, names and bytes in text and back, and the tokens of a line (text_values.cpp).
bool HasIndex(Kind kind);
bool HasKey(Kind kind);
bool HasValue(Kind kind);
std::string Number(Fixed value);
std::string Hex(uint32_t value);
std::string FloatText(float value);
std::string HexByte(uint8_t value);
std::string ArgumentText(const Argument& argument);
std::string NameText(std::string_view name);
[[noreturn]] void Fail(std::size_t line, const std::string& message);
std::vector<Token> Tokenize(std::string_view line, std::size_t number);
std::optional<uint64_t> Integer(std::string_view text);
uint64_t ParseInteger(const Token& token, uint64_t max, std::size_t line, const char* what);
Fixed ParseNumber(const Token& token, std::size_t line);
Argument ParseArgument(const Token& token, std::size_t line);
uint8_t ParseHexByte(const Token& token, std::size_t line);
std::string ShortNumber(Fixed value);
std::string ShortArgumentText(const Argument& argument);
std::string RangeText(const ValueRange& range, const ntdlib::nwsnd::Scale& scale);
bool OnScale(Fixed value, const ntdlib::nwsnd::Scale& scale);
void DecodeBase64(std::string_view text, std::vector<uint8_t>& bytes, std::size_t line);

// The items of a sequence in text and back, and the checks of their commands (text_commands.cpp).
std::vector<std::string> WriteSequence(TextOut& out, const Sequence& sequence, std::size_t number,
                                       const std::vector<bool>& named);
std::optional<std::pair<std::size_t, std::string>> Mp2kProblem(const ntdlib::commands::Sequence& sequence,
                                                               int note_arguments);
std::optional<std::pair<std::size_t, std::string>> DseProblem(const ntdlib::commands::Sequence& sequence);
std::optional<std::pair<std::size_t, std::string>> DriverProblem(const Driver& driver,
                                                                 const ntdlib::commands::Sequence& sequence,
                                                                 std::span<const uint32_t> entries, uint8_t revision);
std::optional<std::pair<std::size_t, std::string>> NtrProblem(const ntdlib::commands::Sequence& sequence);
std::optional<std::string> CommandProblem(const ntdlib::nwsnd::Grammar& grammar, const Command& command);
Command ParseCommand(const std::vector<Token>& tokens, std::size_t line, uint32_t at,
                     std::vector<Reference>& references);
void FinishSequence(SequenceDraft& draft);

// The other sections: the tables, the native archive, effects, scenes, tags, views and timelines (text_sections.cpp).
bool IsTablesWord(const Token& word);
void ParseTablesLine(const std::vector<Token>& tokens, std::size_t line, NativeTables& tables,
                     std::optional<GameSizes>& game_sizes);
void WriteNative(TextOut& out, const NativeArchive& native, const std::vector<std::vector<std::string>>& names);
std::string EffectText(std::size_t bus, const Effect& effect);
std::pair<std::size_t, Effect> ParseEffect(const std::vector<Token>& tokens, std::size_t line);
std::string BusVolumeText(std::size_t bus, float volume);
std::pair<std::size_t, float> ParseBusVolume(const std::vector<Token>& tokens, std::size_t line);
void WriteScene(TextOut& out, std::size_t index, const Scene& scene, const NativeTables* tables);
std::string TagText(std::optional<uint32_t> sound, const std::string& key, const std::string& value,
                    const NativeTables* tables);
std::string SoundMixingText(uint32_t sound, Mixing mixing, const NativeTables* tables);
std::string SoundEngineText(uint32_t sound, Engine engine, const NativeTables* tables);
std::string LengthText(uint32_t sound, const SoundLength& length, const NativeTables* tables);
std::string TrackColourText(const TrackPlace& place, const Colour& colour, const NativeTables* tables);
void WriteTimelines(TextOut& out, const File& file);
void WriteView(TextOut& out, const EditorView& view, const NativeTables* tables);
void ParseView(const std::vector<Token>& tokens, std::size_t line, EditorView& view, std::set<std::string>& given);
void ParseSceneValue(const std::vector<Token>& tokens, std::size_t line, Scene& scene);
FieldDraft ParseField(const std::vector<Token>& tokens, std::size_t line);
uint32_t ItemOfLabel(const NativeArchive& native, std::span<const Sequence> sequences,
                     const std::vector<std::map<std::string, uint32_t>>& locals, const ntdlib::sar::Place& place,
                     const Token& label, std::size_t line);
void ParseTimeline(const std::vector<Token>& tokens, std::size_t number, File& file, TimelineLines& read);

} // namespace hokuto::format::text_parts
