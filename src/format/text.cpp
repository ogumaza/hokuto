// SPDX-License-Identifier: MIT

#include "format/text.h"

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

#include "ntdlib/commands/sequence.h"
#include "ntdlib/commands/units.h"
#include "ntdlib/nwsnd/commands.h"
#include "ntdlib/nwsnd/units.h"
#include "ntdlib/sar/layout.h"
#include "xsflib/binary.h"
#include "xsflib/text.h"
#include "xsflib/utf8.h"

#include "format/codes.h"
#include "format/command_values.h"
#include "format/file.h"
#include "format/kinds.h"
#include "format/model.h"

namespace hokuto::format
{
namespace
{

using ntdlib::commands::Argument;
using ntdlib::commands::Command;
using ntdlib::commands::Fixed;
using ntdlib::commands::Kind;
using ntdlib::commands::kOne;
using ntdlib::commands::Sequence;
using ntdlib::commands::Source;

constexpr std::size_t kBase64Bytes = 57; // the bytes of a line of base64: 76 characters

constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

// The kinds whose number follows their name: a track's, a bus's, an LFO's or a variable's.
bool HasIndex(Kind kind)
{
    switch (kind)
    {
    case Kind::kOpenTrack:
    case Kind::kSend:
    case Kind::kLfoCurve:
    case Kind::kLfoPhase:
    case Kind::kLfoDepth:
    case Kind::kLfoSpeed:
    case Kind::kLfoType:
    case Kind::kLfoRange:
    case Kind::kLfoDelay:
    case Kind::kLfoPeriod:
    case Kind::kSetVariable:
    case Kind::kAddVariable:
    case Kind::kSubtractVariable:
    case Kind::kMultiplyVariable:
    case Kind::kDivideVariable:
    case Kind::kShiftVariable:
    case Kind::kRandomVariable:
    case Kind::kAndVariable:
    case Kind::kOrVariable:
    case Kind::kXorVariable:
    case Kind::kNotVariable:
    case Kind::kModuloVariable:
    case Kind::kCompareEqual:
    case Kind::kCompareGreaterOrEqual:
    case Kind::kCompareGreater:
    case Kind::kCompareLessOrEqual:
    case Kind::kCompareLess:
    case Kind::kCompareNotEqual:
        return true;
    default:
        return false;
    }
}

bool HasValue(Kind kind)
{
    return ntdlib::commands::UnitOf(kind).has_value();
}

bool IsWordCharacter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// The words that a line can hold besides names. A name that is one of them goes in quotes.
bool IsReserved(std::string_view word)
{
    constexpr std::array<std::string_view, 16> kWords = {
        "bytes",  "chunk",  "engine", "field",   "from",     "hok",           "if", "item", "labels",
        "mixing", "native", "over",   "padding", "sequence", "sequence_file", "to",
    };
    return std::find(kWords.begin(), kWords.end(), word) != kWords.end();
}

// A fixed-point number as an exact decimal, such as 100, -1.5 or 0.0078125.
std::string Number(Fixed value)
{
    const bool negative = value < 0;
    const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    std::string text = negative ? "-" : "";
    text += std::to_string(magnitude >> 16);

    // A fraction of 2^16 is the fraction times 5^16, over 10^16: 16 digits at most.
    const uint64_t fraction = magnitude & 0xFFFF;
    if (fraction != 0)
    {
        std::string digits = std::to_string(fraction * 152587890625ULL);
        digits.insert(0, 16 - digits.size(), '0');
        digits.erase(digits.find_last_not_of('0') + 1);
        text += '.';
        text += digits;
    }

    return text;
}

std::string Hex(uint64_t value)
{
    std::array<char, 24> text{};
    std::snprintf(text.data(), text.size(), "0x%llX", static_cast<unsigned long long>(value));

    return text.data();
}

// `value` with `digits` significant digits in printf's %e form: a sign, the digits with a point after the first, then
// e, the exponent's sign and at least two of its digits. printf writes the decimal point of the C locale. The text form
// always uses '.'.
std::string Scientific(float value, int digits)
{
    std::array<char, 32> written{};
    std::snprintf(written.data(), written.size(), "%.*e", digits - 1, static_cast<double>(value));
    std::string text = written.data();
    const std::string_view point = std::localeconv()->decimal_point;
    if (const std::size_t at = text.find(point); !point.empty() && at != std::string::npos)
    {
        text.replace(at, point.size(), ".");
    }

    return text;
}

// Whether xsflib reads `text` back as `value`, to the bit. xsflib reads decimals the same way in every locale.
bool ReadsBack(const std::string& text, float value)
{
    const std::optional<xsflib::DecimalNumber> back = xsflib::ReadDecimal(text);
    return back && back->length == text.size() &&
           std::bit_cast<uint32_t>(static_cast<float>(back->value)) == std::bit_cast<uint32_t>(value);
}

// The text of `value` with the fewest significant digits that read back as it, such as 0.8, -1000 or 1e-45. Nine digits
// always do. A value of at least 0.00001 and below 10^15 in size has no exponent.
std::string FloatText(float value)
{
    std::string text;
    for (int digits = 1; digits <= 9; ++digits)
    {
        text = Scientific(value, digits);
        if (ReadsBack(text, value))
        {
            break;
        }
    }

    // The exponent, then the digits without the sign and the point.
    const std::size_t e = text.find('e');
    int exponent = 0;
    for (std::size_t i = e + 2; i < text.size(); ++i)
    {
        exponent = exponent * 10 + (text[i] - '0');
    }
    exponent = text[e + 1] == '-' ? -exponent : exponent;
    if (exponent < -5 || exponent >= 15)
    {
        return text;
    }

    const bool negative = text[0] == '-';
    std::string digits;
    std::copy_if(text.begin() + (negative ? 1 : 0), text.begin() + static_cast<std::ptrdiff_t>(e),
                 std::back_inserter(digits), [](char c) { return c != '.'; });

    // The point moved by the exponent, with zeros where the digits run out.
    std::string plain = negative ? "-" : "";
    if (exponent < 0)
    {
        plain += "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
    }
    else
    {
        const auto whole = static_cast<std::size_t>(exponent) + 1;
        digits.resize(std::max(digits.size(), whole), '0');
        plain += digits.substr(0, whole) + (digits.size() > whole ? "." + digits.substr(whole) : "");
    }

    return ReadsBack(plain, value) ? plain : text;
}

std::string HexByte(uint8_t value)
{
    std::array<char, 4> text{};
    std::snprintf(text.data(), text.size(), "%02X", value);

    return text.data();
}

std::string ArgumentText(const Argument& argument)
{
    switch (argument.source)
    {
    case Source::kRandom:
        return "random(" + Number(argument.value) + "," + Number(argument.high) + ")";
    case Source::kVariable:
        return "var(" + std::to_string(argument.value) + ")";
    case Source::kValue:
        break;
    }

    return Number(argument.value);
}

// A name as it stands in the text: bare when it holds only letters, digits and underscores and isn't a reserved word,
// or else in quotes. In the quotes, a quote and a backslash take a backslash, and a byte that isn't a printable
// character of UTF-8 takes \x and two hex digits.
std::string NameText(std::string_view name)
{
    if (!name.empty() && std::all_of(name.begin(), name.end(), IsWordCharacter) && !IsReserved(name))
    {
        return std::string(name);
    }

    std::string text = "\"";
    while (!name.empty())
    {
        const auto [code, length] = xsflib::Utf8Character(name);
        if (length == 0 || !xsflib::IsPrintable(code))
        {
            const std::size_t bytes = length == 0 ? 1 : length;
            for (std::size_t i = 0; i < bytes; ++i)
            {
                text += "\\x" + HexByte(static_cast<uint8_t>(name[i]));
            }
            name.remove_prefix(bytes);
            continue;
        }

        if (code == '"' || code == '\\')
        {
            text += '\\';
        }
        text.append(name.substr(0, length));
        name.remove_prefix(length);
    }
    text += '"';

    return text;
}

std::string Base64(std::span<const uint8_t> bytes)
{
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const std::size_t left = std::min<std::size_t>(3, bytes.size() - i);
        uint32_t group = static_cast<uint32_t>(bytes[i]) << 16;
        group |= left > 1 ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0;
        group |= left > 2 ? bytes[i + 2] : 0;
        text += kBase64Alphabet[group >> 18 & 63];
        text += kBase64Alphabet[group >> 12 & 63];
        text += left > 1 ? kBase64Alphabet[group >> 6 & 63] : '=';
        text += left > 2 ? kBase64Alphabet[group & 63] : '=';
    }

    return text;
}

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

// An item's line: its name and the parts that its kind has, then the other parts and the encoder's choices in brackets.
std::string ItemText(const Command& item, const std::vector<std::string>& target_names)
{
    const auto target_text = [&](uint32_t target, bool outside)
    {
        return outside ? "@" + std::to_string(target) : target_names[target];
    };

    std::string text = item.conditional ? "if " : "";
    text += ntdlib::commands::Name(item.kind);
    if (item.kind == Kind::kOther)
    {
        text += " " + Hex(item.encoding.opcode);
    }
    if (HasIndex(item.kind))
    {
        text += " " + std::to_string(item.index);
    }
    if (item.kind == Kind::kNote)
    {
        text += " " + std::to_string(item.key) + " " + Number(item.velocity);
    }
    if (HasValue(item.kind))
    {
        text += " " + ArgumentText(item.value);
    }
    if (item.time)
    {
        text += " over " + ArgumentText(*item.time);
    }
    if (ntdlib::commands::HasTarget(item.kind))
    {
        text += " " + target_text(item.target, item.target_outside);
    }
    if (item.kind == Kind::kData)
    {
        for (const uint8_t byte : item.bytes)
        {
            text += " " + HexByte(byte);
        }
    }

    // The parts that the kind doesn't have. Only an unusual sequence holds them.
    if (!HasIndex(item.kind) && item.index != 0)
    {
        text += " [index " + std::to_string(item.index) + "]";
    }
    if (item.kind != Kind::kNote && item.key != 0)
    {
        text += " [key " + std::to_string(item.key) + "]";
    }
    if (item.kind != Kind::kNote && item.velocity != 0)
    {
        text += " [velocity " + Number(item.velocity) + "]";
    }
    if (!HasValue(item.kind) && item.value != Argument{})
    {
        text += " [value " + ArgumentText(item.value) + "]";
    }
    if (!ntdlib::commands::HasTarget(item.kind) && (item.target != 0 || item.target_outside))
    {
        text += " [target " + (item.target_outside ? "@" : std::string()) + std::to_string(item.target) + "]";
    }
    if (item.kind != Kind::kOther && item.encoding.opcode != 0)
    {
        text += " [opcode " + Hex(item.encoding.opcode) + "]";
    }
    if (item.kind != Kind::kData && !item.bytes.empty())
    {
        text += " [bytes";
        for (const uint8_t byte : item.bytes)
        {
            text += " " + HexByte(byte);
        }
        text += "]";
    }

    // The encoder's choices.
    if (item.encoding.value_bytes != 0)
    {
        text += " [value_bytes " + std::to_string(item.encoding.value_bytes) + "]";
    }
    if (item.encoding.unused_time != 0)
    {
        text += " [unused_time " + Hex(item.encoding.unused_time) + "]";
    }
    if (item.encoding.unused_random)
    {
        text += " [unused_random]";
    }
    if (item.encoding.unused_variable)
    {
        text += " [unused_variable]";
    }

    return text;
}

// Writes sequence `number`: the order of its labels when it isn't the order of their names, then each label and item.
// Each target and each item that `named` marks gets a name. One without a label gets a local one, .L1 onwards in the
// order of the items. Returns each item's name: its first label, a local label, or nothing.
std::vector<std::string> WriteSequence(TextOut& out, const Sequence& sequence, std::size_t number,
                                       const std::vector<bool>& named)
{
    const std::size_t count = sequence.items.size();
    std::vector<std::vector<std::string>> labels(count + 1);
    std::set<std::string> names;
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        if (!names.insert(label.name).second)
        {
            throw std::invalid_argument("sequence " + std::to_string(number) + " has two labels named " + label.name);
        }
        if (label.item > count)
        {
            throw std::invalid_argument("label " + label.name + " of sequence " + std::to_string(number) +
                                        " is past its end");
        }

        labels[label.item].push_back(NameText(label.name));
    }

    std::vector<bool> targeted(count + 1);
    for (const Command& item : sequence.items)
    {
        if (ntdlib::commands::HasTarget(item.kind) && !item.target_outside)
        {
            if (item.target > count)
            {
                throw std::invalid_argument("a command of sequence " + std::to_string(number) + " goes past its end");
            }

            targeted[item.target] = true;
        }
    }

    std::vector<std::string> target_names(count + 1);
    std::vector<std::string> locals(count + 1);
    int next_local = 1;
    for (std::size_t i = 0; i <= count; ++i)
    {
        if (!labels[i].empty())
        {
            target_names[i] = labels[i].front();
        }
        else if (targeted[i] || (i < named.size() && named[i]))
        {
            locals[i] = ".L" + std::to_string(next_local++);
            target_names[i] = locals[i];
        }
    }

    out.Line(0, "sequence " + std::to_string(number));
    std::vector<std::string> order;
    for (const ntdlib::commands::Label& label : sequence.labels)
    {
        order.push_back(label.name);
    }
    if (!std::is_sorted(order.begin(), order.end()))
    {
        std::string line = "labels";
        for (const std::string& name : order)
        {
            line += " " + NameText(name);
        }
        out.Line(1, line);
    }

    for (std::size_t i = 0; i <= count; ++i)
    {
        for (const std::string& label : labels[i])
        {
            out.Line(1, label + ":");
        }
        if (!locals[i].empty())
        {
            out.Line(1, locals[i] + ":");
        }
        if (i < count)
        {
            out.Line(2, ItemText(sequence.items[i], target_names));
        }
    }

    return target_names;
}

[[noreturn]] void Fail(std::size_t line, const std::string& message)
{
    throw xsflib::FormatError("Line " + std::to_string(line) + ": " + message + ".");
}

struct Token
{
    bool Is(std::string_view word) const
    {
        return !quoted && text == word;
    }

    std::string text;
    bool quoted = false;
};

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }

    return -1;
}

// The tokens of a line: bare words, quoted names, and the marks [, ] and :. A bare word runs through any parentheses. A
// # outside quotes starts a comment.
std::vector<Token> Tokenize(std::string_view line, std::size_t number)
{
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < line.size())
    {
        const char c = line[i];
        if (c == ' ' || c == '\t')
        {
            ++i;
            continue;
        }
        if (c == '#')
        {
            break;
        }
        if (c == '[' || c == ']' || c == ':')
        {
            tokens.push_back({std::string(1, c), false});
            ++i;
            continue;
        }

        if (c == '"')
        {
            Token token{{}, true};
            ++i;
            while (true)
            {
                if (i >= line.size())
                {
                    Fail(number, "a name has no closing quote");
                }

                const char d = line[i++];
                if (d == '"')
                {
                    break;
                }
                if (d != '\\')
                {
                    token.text += d;
                    continue;
                }

                if (i < line.size() && (line[i] == '"' || line[i] == '\\'))
                {
                    token.text += line[i++];
                }
                else if (i + 2 < line.size() && line[i] == 'x' && HexDigit(line[i + 1]) >= 0 &&
                         HexDigit(line[i + 2]) >= 0)
                {
                    token.text += static_cast<char>(HexDigit(line[i + 1]) * 16 + HexDigit(line[i + 2]));
                    i += 3;
                }
                else
                {
                    Fail(number, "a name holds a backslash that isn't \\\", \\\\ or \\x and two hex digits");
                }
            }
            tokens.push_back(std::move(token));
            continue;
        }

        Token token;
        int depth = 0;
        while (i < line.size())
        {
            const char d = line[i];
            if (depth == 0 && (d == ' ' || d == '\t' || d == '#' || d == '[' || d == ']' || d == ':' || d == '"'))
            {
                break;
            }
            if (d == '(')
            {
                ++depth;
            }
            else if (d == ')' && --depth < 0)
            {
                Fail(number, "a ) has no (");
            }
            token.text += d;
            ++i;
        }
        if (depth != 0)
        {
            Fail(number, "a ( has no )");
        }
        tokens.push_back(std::move(token));
    }

    return tokens;
}

// A whole number: decimal, or hex after 0x.
std::optional<uint64_t> Integer(std::string_view text)
{
    uint64_t value = 0;
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const std::string_view digits = hex ? text.substr(2) : text;
    if (digits.empty())
    {
        return std::nullopt;
    }

    const uint64_t base = hex ? 16 : 10;
    for (const char c : digits)
    {
        const int digit = HexDigit(c);
        if (digit < 0 || static_cast<uint64_t>(digit) >= base ||
            value > (UINT64_MAX - static_cast<uint64_t>(digit)) / base)
        {
            return std::nullopt;
        }

        value = value * base + static_cast<uint64_t>(digit);
    }

    return value;
}

uint64_t ParseInteger(const Token& token, uint64_t max, std::size_t line, const char* what)
{
    const std::optional<uint64_t> value = token.quoted ? std::nullopt : Integer(token.text);
    if (!value || *value > max)
    {
        Fail(line,
             std::string(what) + " has to be a whole number from 0 to " + std::to_string(max) + ", not " + token.text);
    }

    return *value;
}

// A decimal number as fixed point, to the nearest 2^-16. A value halfway between two goes away from zero.
std::optional<Fixed> Decimal(std::string_view text)
{
    const bool negative = !text.empty() && text[0] == '-';
    if (negative)
    {
        text.remove_prefix(1);
    }

    const std::size_t point = text.find('.');
    const std::string_view whole = text.substr(0, point);
    const std::string_view fraction = point == std::string_view::npos ? std::string_view() : text.substr(point + 1);
    if (whole.empty() && fraction.empty())
    {
        return std::nullopt;
    }

    uint64_t magnitude = 0;
    for (const char c : whole)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }

        magnitude = magnitude * 10 + static_cast<uint64_t>(c - '0');
        if (magnitude > uint64_t{1} << 47)
        {
            return std::nullopt;
        }
    }

    // The fraction's first 17 bits, by doubling its decimal digits. The 17th bit rounds.
    std::vector<int> digits;
    for (const char c : fraction)
    {
        if (c < '0' || c > '9')
        {
            return std::nullopt;
        }

        digits.push_back(c - '0');
    }

    uint64_t bits = 0;
    for (int bit = 0; bit < 17; ++bit)
    {
        int carry = 0;
        for (auto d = digits.rbegin(); d != digits.rend(); ++d)
        {
            const int doubled = *d * 2 + carry;
            *d = doubled % 10;
            carry = doubled / 10;
        }
        bits = bits << 1 | static_cast<uint64_t>(carry);
    }

    magnitude = (magnitude << 16) + (bits >> 1) + (bits & 1);
    if (magnitude > (uint64_t{1} << 63) - (negative ? 0 : 1))
    {
        return std::nullopt;
    }

    return negative ? static_cast<Fixed>(0 - magnitude) : static_cast<Fixed>(magnitude);
}

Fixed ParseNumber(const Token& token, std::size_t line)
{
    const std::optional<Fixed> value = token.quoted ? std::nullopt : Decimal(token.text);
    if (!value)
    {
        Fail(line, "expected a number, not " + token.text);
    }

    return *value;
}

// A value, random(low,high) or var(n).
Argument ParseArgument(const Token& token, std::size_t line)
{
    Argument argument;
    const std::string_view text = token.text;
    const auto inside = [&](std::string_view head)
    {
        return text.substr(head.size(), text.size() - head.size() - 1);
    };

    if (!token.quoted && text.starts_with("random(") && text.ends_with(")"))
    {
        const std::string_view range = inside("random(");
        const std::size_t comma = range.find(',');
        const std::optional<Fixed> low = Decimal(range.substr(0, comma));
        const std::optional<Fixed> high =
            comma == std::string_view::npos ? std::nullopt : Decimal(range.substr(comma + 1));
        if (!low || !high)
        {
            Fail(line, "expected random(low,high), not " + token.text);
        }

        argument.source = Source::kRandom;
        argument.value = *low;
        argument.high = *high;
    }
    else if (!token.quoted && text.starts_with("var(") && text.ends_with(")"))
    {
        // A negative number reads too. The check of the command's values then gives the numbers that its kind reads.
        std::string_view number = inside("var(");
        const bool negative = number.starts_with('-');
        const std::optional<uint64_t> variable = Integer(negative ? number.substr(1) : number);
        if (!variable || *variable > 0xFFFFFFFF)
        {
            Fail(line, "expected var(n), not " + token.text);
        }

        argument.source = Source::kVariable;
        argument.value = negative ? -static_cast<int64_t>(*variable) : static_cast<int64_t>(*variable);
    }
    else
    {
        argument.value = ParseNumber(token, line);
    }

    return argument;
}

uint8_t ParseHexByte(const Token& token, std::size_t line)
{
    if (token.quoted || token.text.size() != 2 || HexDigit(token.text[0]) < 0 || HexDigit(token.text[1]) < 0)
    {
        Fail(line, "expected a byte in two hex digits, not " + token.text);
    }

    return static_cast<uint8_t>(HexDigit(token.text[0]) * 16 + HexDigit(token.text[1]));
}

// `value` as the shortest decimal that the text form reads back as `value`: 0.01 where Number gives 0.0099945068359375.
// A value beyond a double's exact range takes Number's digits.
std::string ShortNumber(Fixed value)
{
    const std::string exact = Number(value);
    const std::size_t point = exact.find('.');
    const uint64_t magnitude = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    if (point == std::string::npos || magnitude > uint64_t{1} << 52)
    {
        return exact;
    }

    // printf writes the decimal point of the C locale. The text form always uses '.'.
    const std::string_view locale_point = std::localeconv()->decimal_point;
    for (std::size_t digits = 1; point + 1 + digits < exact.size(); ++digits)
    {
        std::array<char, 48> written{};
        std::snprintf(written.data(), written.size(), "%.*f", static_cast<int>(digits),
                      static_cast<double>(value) / static_cast<double>(kOne));
        std::string text = written.data();
        if (const std::size_t at = text.find(locale_point); !locale_point.empty() && at != std::string::npos)
        {
            text.replace(at, locale_point.size(), ".");
        }
        if (Decimal(text) == value)
        {
            return text;
        }
    }

    return exact;
}

// `argument` as ArgumentText gives it, with each number as ShortNumber gives it.
std::string ShortArgumentText(const Argument& argument)
{
    switch (argument.source)
    {
    case Source::kRandom:
        return "random(" + ShortNumber(argument.value) + "," + ShortNumber(argument.high) + ")";
    case Source::kVariable:
        return "var(" + std::to_string(argument.value) + ")";
    case Source::kValue:
        break;
    }

    return ShortNumber(argument.value);
}

// How the text form words the values from `range` on `scale`: "a whole number from 0 to 255", or "a number from -1 to
// 0.984375 in steps of 0.015625".
std::string RangeText(const ValueRange& range, const ntdlib::nwsnd::Scale& scale)
{
    const std::string ends = " from " + Number(range.lowest) + " to " + Number(range.highest);
    if (scale.step == kOne && scale.offset % kOne == 0)
    {
        return "a whole number" + ends;
    }

    return "a number" + ends + " in steps of " + Number(scale.step);
}

// Whether `value` is one of the values of `scale`.
bool OnScale(Fixed value, const ntdlib::nwsnd::Scale& scale)
{
    return ntdlib::nwsnd::ToUnit(ntdlib::nwsnd::FromUnit(value, scale), scale) == value;
}

// Why the console's bytes can't hold the values of `command` as the text form gives them, by the rules of `grammar`: a
// value out of its range, between two values of the console's scale, or from a source that the kind doesn't take. An
// example is "wait has to be a whole number from 0 to 4294967295, not -5". Nothing where the bytes hold them. A kData
// item and a command of another kind (kOther) aren't checked: they give the console's bytes.
std::optional<std::string> CommandProblem(const ntdlib::nwsnd::Grammar& grammar, const Command& command)
{
    using ntdlib::nwsnd::Scale;
    if (command.kind == Kind::kData || command.kind == Kind::kOther)
    {
        return std::nullopt;
    }

    // The encoder takes a value to the console's nearest. A value between two takes another value without a word.
    const Scale value_scale = ntdlib::nwsnd::ScaleOf(command.kind, grammar);

    const auto exact = [](const Argument& argument, const Scale& scale)
    {
        return argument.source == Source::kVariable ||
               (OnScale(argument.value, scale) &&
                (argument.source != Source::kRandom || OnScale(argument.high, scale)));
    };

    const bool note = command.kind == Kind::kNote;
    const bool whole_velocity = !note || OnScale(command.velocity, ntdlib::nwsnd::kLevelScale);
    const bool exact_value = !ntdlib::commands::UnitOf(command.kind) || exact(command.value, value_scale);
    const bool exact_time = !command.time || exact(*command.time, ntdlib::nwsnd::kTicksScale);
    if (whole_velocity && exact_value && exact_time && Holds(grammar, command))
    {
        return std::nullopt;
    }

    // The part that the bytes can't hold, with the values that they hold there.
    const std::string name(ntdlib::commands::Name(command.kind));
    if (note)
    {
        Command probe = command;
        probe.value = Argument{};
        const std::optional<ValueRange> range = HeldVelocities(grammar, probe);
        if (range && (!whole_velocity || command.velocity < range->lowest || command.velocity > range->highest))
        {
            return "a note's velocity has to be " + RangeText(*range, ntdlib::nwsnd::kLevelScale) + ", not " +
                   ShortNumber(command.velocity);
        }
    }

    for (const bool time : {false, true})
    {
        if (time && !command.time)
        {
            continue;
        }

        // The argument's range with a value of 0 in the other argument.
        const Argument& argument = time ? *command.time : command.value;
        Command probe = command;
        if (time)
        {
            probe.value = Argument{};
        }
        else if (probe.time)
        {
            probe.time = Argument{};
        }
        const std::optional<ValueRange> range = HeldRange(grammar, probe, argument.source, time);
        const std::string what = time ? name + "'s time" : note ? "a note's length" : name;
        if (!range)
        {
            // A kind that takes a value, but not from this source, or no time.
            if (time && !HeldRange(grammar, probe, Source::kValue, true))
            {
                return name + " takes no time, not over " + ShortArgumentText(argument);
            }
            if (argument.source != Source::kValue && HeldRange(grammar, probe, Source::kValue, time))
            {
                return what + " takes no " + (argument.source == Source::kRandom ? "random value" : "variable") +
                       ", not " + ShortArgumentText(argument);
            }
            continue;
        }

        const bool variable = argument.source == Source::kVariable;
        const Scale scale = variable ? Scale{1, 0} : time ? ntdlib::nwsnd::kTicksScale : value_scale;
        const auto fits = [&](int64_t value)
        {
            return value >= range->lowest && value <= range->highest && (variable || OnScale(value, scale));
        };
        if (fits(argument.value) && (argument.source != Source::kRandom || fits(argument.high)))
        {
            continue;
        }

        if (variable)
        {
            return what + " has to read a variable from " + std::to_string(range->lowest) + " to " +
                   std::to_string(range->highest) + ", not " + ShortArgumentText(argument);
        }

        return what + " has to be " + RangeText(*range, scale) + ", not " + ShortArgumentText(argument);
    }

    // Another reason, such as a part that the kind doesn't have. The export gives it.
    return std::nullopt;
}

// A reference to a label that a command makes before the label may be known.
struct Reference
{
    std::size_t line = 0;
    uint32_t item = 0;
    Token name;
};

// The parts of a command from its tokens. A target by name goes into `references`.
Command ParseCommand(const std::vector<Token>& tokens, std::size_t line, uint32_t at,
                     std::vector<Reference>& references)
{
    Command item;
    std::size_t t = 0;
    if (tokens[t].Is("if"))
    {
        item.conditional = true;
        ++t;
    }
    if (t >= tokens.size())
    {
        Fail(line, "if has no command");
    }

    const std::optional<Kind> kind = tokens[t].quoted ? std::nullopt : ntdlib::commands::KindNamed(tokens[t].text);
    if (!kind)
    {
        Fail(line, "no command is named " + tokens[t].text);
    }

    item.kind = *kind;
    const std::string name = tokens[t++].text;
    const auto next = [&]() -> const Token&
    {
        if (t >= tokens.size() || tokens[t].Is("["))
        {
            Fail(line, name + " lacks a part");
        }

        return tokens[t++];
    };

    if (item.kind == Kind::kOther)
    {
        item.encoding.opcode = static_cast<uint16_t>(ParseInteger(next(), 0xFFFF, line, "an opcode"));
    }
    if (HasIndex(item.kind))
    {
        item.index = static_cast<uint8_t>(ParseInteger(next(), 255, line, "a number"));
    }
    if (item.kind == Kind::kNote)
    {
        item.key = static_cast<uint8_t>(ParseInteger(next(), 127, line, "a note's key"));
        item.velocity = ParseNumber(next(), line);
    }
    if (HasValue(item.kind))
    {
        item.value = ParseArgument(next(), line);
    }
    if (t < tokens.size() && tokens[t].Is("over"))
    {
        ++t;
        item.time = ParseArgument(next(), line);
    }
    if (ntdlib::commands::HasTarget(item.kind))
    {
        const Token& target = next();
        if (!target.quoted && target.text.starts_with("@"))
        {
            item.target_outside = true;
            item.target =
                static_cast<uint32_t>(ParseInteger(Token{target.text.substr(1), false}, 0xFFFFFFFF, line, "an offset"));
        }
        else
        {
            references.push_back({line, at, target});
        }
    }
    if (item.kind == Kind::kData)
    {
        while (t < tokens.size() && !tokens[t].Is("["))
        {
            item.bytes.push_back(ParseHexByte(tokens[t++], line));
        }
    }

    // The bracketed parts.
    while (t < tokens.size())
    {
        if (!tokens[t].Is("["))
        {
            Fail(line, "unexpected " + tokens[t].text);
        }

        const std::size_t open = ++t;
        while (t < tokens.size() && !tokens[t].Is("]"))
        {
            ++t;
        }
        if (t >= tokens.size() || t == open)
        {
            Fail(line, "a [ has no ] or is empty");
        }

        const std::string& part = tokens[open].text;
        const std::span<const Token> args(tokens.data() + open + 1, t - open - 1);
        ++t;
        const auto one = [&]() -> const Token&
        {
            if (args.size() != 1)
            {
                Fail(line, "[" + part + "] takes one value");
            }

            return args[0];
        };

        if (part == "index")
        {
            item.index = static_cast<uint8_t>(ParseInteger(one(), 255, line, "an index"));
        }
        else if (part == "key")
        {
            // A note's key holds 0 to 127. Its main parts take the same keys.
            const bool note = item.kind == Kind::kNote;
            item.key =
                static_cast<uint8_t>(ParseInteger(one(), note ? 127 : 255, line, note ? "a note's key" : "a key"));
        }
        else if (part == "velocity")
        {
            item.velocity = ParseNumber(one(), line);
        }
        else if (part == "value")
        {
            item.value = ParseArgument(one(), line);
        }
        else if (part == "target")
        {
            const Token& target = one();
            item.target_outside = target.text.starts_with("@");
            item.target = static_cast<uint32_t>(ParseInteger(
                Token{item.target_outside ? target.text.substr(1) : target.text, false}, 0xFFFFFFFF, line, "a target"));
        }
        else if (part == "opcode")
        {
            item.encoding.opcode = static_cast<uint16_t>(ParseInteger(one(), 0xFFFF, line, "an opcode"));
        }
        else if (part == "bytes")
        {
            for (const Token& byte : args)
            {
                item.bytes.push_back(ParseHexByte(byte, line));
            }
        }
        else if (part == "value_bytes")
        {
            item.encoding.value_bytes = static_cast<uint8_t>(ParseInteger(one(), 255, line, "value_bytes"));
        }
        else if (part == "unused_time")
        {
            item.encoding.unused_time = static_cast<uint8_t>(ParseInteger(one(), 255, line, "unused_time"));
        }
        else if (part == "unused_random" && args.empty())
        {
            item.encoding.unused_random = true;
        }
        else if (part == "unused_variable" && args.empty())
        {
            item.encoding.unused_variable = true;
        }
        else
        {
            Fail(line, "no part of a command is written [" + part + (args.empty() ? "" : " ...") + "]");
        }
    }

    return item;
}

// The bytes of a line of base64.
void DecodeBase64(std::string_view text, std::vector<uint8_t>& bytes, std::size_t line)
{
    if (text.size() % 4 != 0)
    {
        Fail(line, "a line of base64 has to hold a multiple of 4 characters");
    }

    for (std::size_t i = 0; i < text.size(); i += 4)
    {
        uint32_t group = 0;
        int padding = 0;
        for (std::size_t k = 0; k < 4; ++k)
        {
            const char c = text[i + k];
            const std::size_t value = kBase64Alphabet.find(c);
            if (c == '=' && i + 4 == text.size() && k >= 2)
            {
                ++padding;
            }
            else if (value == std::string_view::npos || padding != 0)
            {
                Fail(line, "a line of base64 holds a character outside base64");
            }

            group = group << 6 | static_cast<uint32_t>(value == std::string_view::npos ? 0 : value);
        }

        bytes.push_back(static_cast<uint8_t>(group >> 16));
        if (padding < 2)
        {
            bytes.push_back(static_cast<uint8_t>(group >> 8));
        }
        if (padding < 1)
        {
            bytes.push_back(static_cast<uint8_t>(group));
        }
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
            Fail(line, "the base64 holds more than " + std::to_string(count) + " bytes");
        }

        return bytes;
    }

private:
    std::vector<std::string_view> lines_;
    std::size_t next_ = 0;
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

void FinishSequence(SequenceDraft& draft)
{
    Sequence& sequence = draft.sequence;
    for (const Reference& reference : draft.references)
    {
        const bool local = !reference.name.quoted && reference.name.text.starts_with(".");
        const std::map<std::string, uint32_t>& names = local ? draft.locals : draft.labels;
        const auto found = names.find(reference.name.text);
        if (found == names.end())
        {
            Fail(reference.line, "no label is named " + reference.name.text);
        }

        sequence.items[reference.item].target = found->second;
    }

    // The labels in the order that the labels line gives, or else in the order of their names.
    if (draft.order.empty())
    {
        for (const auto& [name, item] : draft.labels)
        {
            sequence.labels.push_back({name, item});
        }

        return;
    }

    if (draft.order.size() != draft.labels.size())
    {
        Fail(draft.order_line, "the labels line has to name each label once");
    }
    for (const std::string& name : draft.order)
    {
        const auto found = draft.labels.find(name);
        if (found == draft.labels.end())
        {
            Fail(draft.order_line, "the labels line names " + name + ", and the sequence has no such label");
        }

        sequence.labels.push_back({name, found->second});
    }
}

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
        Fail(line, "Hokuto has no kind of sound named " + tokens[2].text);
    }

    sound.kind = *kind;
    std::set<std::string> given;
    std::size_t t = 3;
    while (t < tokens.size())
    {
        const Token& key = tokens[t++];
        if (!given.insert(key.text).second)
        {
            Fail(line, "the sound gives " + key.text + " twice");
        }

        // The number after the key.
        const auto number = [&](uint64_t max)
        {
            if (t >= tokens.size())
            {
                Fail(line, key.text + " takes a number");
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
        Fail(line, "group " + tokens[1].text + " gives no game_size, and the native archive gives its game_size");
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
            Fail(line, "a line of " + word.text + " holds its name");
        }

        std::vector<std::string>& names = word.Is("bank")    ? tables.banks
                                          : word.Is("group") ? tables.groups
                                                             : tables.wave_archives;
        names.push_back(tokens[1].text);
    }
}

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

// The native archive's game size, the game's build of NW4R SND on the Wii, its pieces, each with its number in a
// comment, then its fields and its tables.
void WriteNative(TextOut& out, const NativeArchive& native, const std::vector<std::vector<std::string>>& names)
{
    if (const std::optional<std::string> problem = GameSizesProblem(native))
    {
        throw std::invalid_argument("the game's sizes " + *problem);
    }

    out.Line(0, "native " + std::string(native.wii ? kWiiArchiveName : NameOf(native.flavor)));
    if (native.game_sizes)
    {
        out.Line(1, "game_size " + std::to_string(native.game_sizes->archive));
    }
    if (native.wii_build)
    {
        if (!native.wii)
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
            out.Line(1, "sequence_file " + std::to_string(file->sequence) + " version " + Hex(file->version) + number);
        }
        else
        {
            out.Line(1, "padding " + std::to_string(std::get<ntdlib::sar::Padding>(piece).alignment) + number);
        }
    }

    for (const ntdlib::sar::Field& field : native.fields)
    {
        out.Line(1, "field " + PlaceText(field.at, native, names) + " from " + PlaceText(field.from, native, names) +
                        " to " + PlaceText(field.to, native, names));
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
        throw std::invalid_argument("an effect on aux bus " + std::string(1, static_cast<char>('A' + bus)) + " " +
                                    *problem);
    }

    std::string text =
        "effect " + std::string(1, static_cast<char>('A' + bus)) + " " + std::string(NameOf(effect.kind));
    const std::span<const std::string_view> names = ParameterNamesOf(effect.kind);
    for (std::size_t p = 0; p < names.size(); ++p)
    {
        text += " " + std::string(names[p]) + " " + FloatText(effect.parameters[p]);
    }

    return text;
}

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

// An effect's line: effect, a bus from A to C and a kind of effect, then its parameters' names and values in any order.
// A parameter that the line leaves out is 0. nn::audio starts each parameter at 0. Returns the bus and the effect.
std::pair<std::size_t, Effect> ParseEffect(const std::vector<Token>& tokens, std::size_t line)
{
    const bool bus_named = tokens.size() >= 3 && !tokens[1].quoted && tokens[1].text.size() == 1 &&
                           tokens[1].text[0] >= 'A' && tokens[1].text[0] < static_cast<char>('A' + kAuxBuses);
    if (!bus_named)
    {
        Fail(line, "an effect's line is effect, a bus from A to C and a kind of effect, then its parameters");
    }

    const auto bus = static_cast<std::size_t>(tokens[1].text[0] - 'A');
    const std::optional<EffectKind> kind = tokens[2].quoted ? std::nullopt : EffectKindNamed(tokens[2].text);
    if (!kind)
    {
        Fail(line, "Hokuto has no effect named " + tokens[2].text);
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
            Fail(line, "the effect gives " + name.text + " twice");
        }
        if (t + 1 >= tokens.size())
        {
            Fail(line, name.text + " takes a number");
        }

        effect.parameters[p] = ParseFloat(tokens[t + 1], line);
        given[p] = true;
    }
    if (const std::optional<std::string> problem = EffectProblem(effect))
    {
        Fail(line, "the effect " + *problem);
    }

    return {bus, effect};
}

// An aux bus's volume's line: bus_volume, the bus and the volume as a gain.
std::string BusVolumeText(std::size_t bus, float volume)
{
    const std::string name(1, static_cast<char>('A' + bus));
    if (const std::optional<std::string> problem = AuxVolumeProblem(volume))
    {
        throw std::invalid_argument("aux bus " + name + "'s volume " + *problem);
    }

    return "bus_volume " + name + " " + FloatText(volume);
}

// An aux bus's volume's line: bus_volume, a bus from A to C and the volume. Returns the bus and the volume.
std::pair<std::size_t, float> ParseBusVolume(const std::vector<Token>& tokens, std::size_t line)
{
    const bool bus_named = tokens.size() == 3 && !tokens[1].quoted && tokens[1].text.size() == 1 &&
                           tokens[1].text[0] >= 'A' && tokens[1].text[0] < static_cast<char>('A' + kAuxBuses);
    if (!bus_named)
    {
        Fail(line, "a bus's volume's line is bus_volume, a bus from A to C and the volume");
    }

    const auto bus = static_cast<std::size_t>(tokens[1].text[0] - 'A');
    const float volume = ParseFloat(tokens[2], line);
    if (const std::optional<std::string> problem = AuxVolumeProblem(volume))
    {
        Fail(line, "the volume " + *problem);
    }

    return {bus, volume};
}

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

// The lines of scene `index`: its sound and its name, then a line for each starting value and for each change. Where
// `tables` name the sound, a comment gives its name.
void WriteScene(TextOut& out, std::size_t index, const Scene& scene, const NativeTables* tables)
{
    if (const std::optional<std::string> problem = SceneProblem(scene))
    {
        throw std::invalid_argument("scene " + std::to_string(index) + " " + *problem);
    }

    std::string line = "scene " + std::to_string(scene.sound) + " " + NameText(scene.name);
    if (tables != nullptr && scene.sound < tables->sounds.size() && !tables->sounds[scene.sound].name.empty())
    {
        line += " # " + NameText(tables->sounds[scene.sound].name);
    }
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
    if (sound && tables != nullptr && *sound < tables->sounds.size() && !tables->sounds[*sound].name.empty())
    {
        line += " # " + NameText(tables->sounds[*sound].name);
    }

    return line;
}

// The line of a sound's mixing mode: sound_mixing, the sound and the mode's name. Where `tables` name the sound, a
// comment gives its name.
std::string SoundMixingText(uint32_t sound, Mixing mixing, const NativeTables* tables)
{
    std::string line = "sound_mixing " + std::to_string(sound) + " " + std::string(NameOf(mixing));
    if (tables != nullptr && sound < tables->sounds.size() && !tables->sounds[sound].name.empty())
    {
        line += " # " + NameText(tables->sounds[sound].name);
    }

    return line;
}

// The line of a sound's engine: sound_engine, the sound and the engine's name. Where `tables` name the sound, a comment
// gives its name.
std::string SoundEngineText(uint32_t sound, Engine engine, const NativeTables* tables)
{
    std::string line = "sound_engine " + std::to_string(sound) + " " + std::string(NameOf(engine));
    if (tables != nullptr && sound < tables->sounds.size() && !tables->sounds[sound].name.empty())
    {
        line += " # " + NameText(tables->sounds[sound].name);
    }

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
    if (tables != nullptr && sound < tables->sounds.size() && !tables->sounds[sound].name.empty())
    {
        line += " # " + NameText(tables->sounds[sound].name);
    }

    return line;
}

// The line of a track's colour: the sound, the track, and the colour's red, green and blue. Where `tables` name the
// sound, a comment gives its name.
std::string TrackColourText(const TrackPlace& place, const Colour& colour, const NativeTables* tables)
{
    std::string line = "track_colour " + std::to_string(place.sound) + " " + std::to_string(place.track) + " " +
                       std::to_string(colour.red) + " " + std::to_string(colour.green) + " " +
                       std::to_string(colour.blue);
    if (tables != nullptr && place.sound < tables->sounds.size() && !tables->sounds[place.sound].name.empty())
    {
        line += " # " + NameText(tables->sounds[place.sound].name);
    }

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

// The names of the windows that a view can put in front, by their numbers in ViewFront.
constexpr std::array<std::string_view, 3> kFrontNames = {"mixer", "key_editor", "list_editor"};

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
    if (tables != nullptr && view.sound < tables->sounds.size() && !tables->sounds[view.sound].name.empty())
    {
        sound += " # " + NameText(tables->sounds[view.sound].name);
    }

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
        Fail(line, "the view gives its " + part + " twice");
    }

    const auto takes = [&tokens, &part, line](std::size_t values, const std::string& what)
    {
        if (tokens.size() != values + 2)
        {
            Fail(line, "the view's " + part + " takes " + what);
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
            Fail(line, "the view's " + part + " takes the numbers of tracks");
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
            Fail(line, "Hokuto has no kind of command named " + tokens[2].text);
        }

        view.lanes.push_back({*kind, static_cast<uint8_t>(ParseInteger(tokens[3], 0xFF, line, "a bus or an LFO"))});
    }
    else
    {
        Fail(line, "the view has no part named " + part);
    }
}

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

// The piece of a place at the archive's end, until the number of pieces is known.
constexpr uint32_t kEndPiece = 0xFFFFFFFF;

// A place of a field's line, with the label that names its item, if a label does.
struct PlaceDraft
{
    ntdlib::sar::Place place;
    std::optional<Token> label;
};

// A field's line, with the labels that name its places' items.
struct FieldDraft
{
    ntdlib::sar::Field field;
    std::optional<Token> from_label;
    std::optional<Token> to_label;
    std::size_t line = 0;
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

// A field's line: field, where the field lies, from and a place, then to and a place.
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
    if (t != tokens.size())
    {
        Fail(line, kForm);
    }

    draft.field = {at.place, from.place, to.place};
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

// The lines of the timelines that a reader has read: the line of each timeline's bars, by its sequence, and the line of
// none.
struct TimelineLines
{
    std::map<uint32_t, std::size_t> bars;
    std::size_t none = 0;
};

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
                Fail(number, "a track's hints give " + tokens[t].text + " once");
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
    found->second.tracks.push_back(std::move(track));
}

} // namespace

std::string WriteText(const File& file)
{
    if (const std::optional<std::size_t> sequence = UnfiledChoices(file))
    {
        throw std::invalid_argument(
            "sequence " + std::to_string(*sequence) +
            " holds the encoder's choices, and no sequence file of the native archive or its group files names it");
    }

    TextOut out;
    out.Line(0, "hok " + std::to_string(kMajorVersion) + "." + std::to_string(kMinorVersion));
    out.Line(0, "engine " + std::string(NameOf(file.engine)));
    out.Line(0, "mixing " + std::string(NameOf(file.mixing)));
    const NativeTables* tables = file.native && file.native->tables ? &*file.native->tables : nullptr;
    for (const auto& [sound, mixing] : file.sound_mixing)
    {
        out.Line(0, SoundMixingText(sound, mixing, tables));
    }
    for (const auto& [sound, engine] : file.sound_engines)
    {
        out.Line(0, SoundEngineText(sound, engine, tables));
    }
    for (const auto& [key, value] : file.tags)
    {
        out.Line(0, TagText(std::nullopt, key, value, tables));
    }
    for (const auto& [sound, tags] : file.sound_tags)
    {
        for (const auto& [key, value] : tags)
        {
            out.Line(0, TagText(sound, key, value, tables));
        }
    }
    for (const auto& [sound, length] : file.lengths)
    {
        out.Line(0, LengthText(sound, length, tables));
    }
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        for (const Effect& effect : file.aux_effects[bus])
        {
            out.Line(0, EffectText(bus, effect));
        }
    }
    for (std::size_t bus = 0; bus < kAuxBuses; ++bus)
    {
        if (file.aux_volumes[bus] != 1.0f)
        {
            out.Line(0, BusVolumeText(bus, file.aux_volumes[bus]));
        }
    }
    for (std::size_t s = 0; s < file.scenes.size(); ++s)
    {
        out.Line(0, "");
        WriteScene(out, s, file.scenes[s], tables);
    }
    if (!file.track_colours.empty())
    {
        out.Line(0, "");
    }
    for (const auto& [place, colour] : file.track_colours)
    {
        out.Line(0, TrackColourText(place, colour, tables));
    }
    if (file.view)
    {
        out.Line(0, "");
        WriteView(out, *file.view, tables);
    }
    if (file.timelines)
    {
        out.Line(0, "");
        WriteTimelines(out, file);
    }

    // The items that the native archive's fields name by a label: each item but a bytecode's first. A file in the model
    // form keeps no fields.
    const NativeArchive* native = file.native && file.form == ArchiveForm::kNative ? &*file.native : nullptr;
    std::vector<std::vector<bool>> named(file.sequences.size());
    for (const ntdlib::sar::Field& field : native ? native->fields : std::vector<ntdlib::sar::Field>{})
    {
        for (const ntdlib::sar::Place& place : {field.from, field.to})
        {
            const std::vector<ntdlib::sar::Piece>& pieces = *file.native->pieces;
            const auto* piece = !place.sequence && place.piece < pieces.size()
                                    ? std::get_if<ntdlib::sar::SequencePiece>(&pieces[place.piece])
                                    : nullptr;
            const std::size_t sequence = place.sequence ? place.piece : piece ? piece->sequence : named.size();
            if (place.item && place.offset > 0 && sequence < named.size())
            {
                std::vector<bool>& items = named[sequence];
                items.resize(std::max<std::size_t>(items.size(), std::size_t{place.offset} + 1));
                items[place.offset] = true;
            }
        }
    }

    std::vector<std::vector<std::string>> names;
    for (std::size_t s = 0; s < file.sequences.size(); ++s)
    {
        out.Line(0, "");
        names.push_back(WriteSequence(out, *file.sequences[s], s, named[s]));
    }
    if (native)
    {
        out.Line(0, "");
        WriteNative(out, *native, names);
    }

    const auto write_chunk = [&](std::string_view id, const std::vector<uint8_t>& chunk)
    {
        out.Line(0, "");
        out.Line(0, "chunk " + std::string(id) + " " + std::to_string(chunk.size()));
        out.Bytes(1, chunk);
    };
    if (native && !native->group_files.empty())
    {
        write_chunk(kGroupFilesChunk, GroupFilesChunk(file));
    }
    if (native && !native->streams.empty())
    {
        write_chunk(kStreamsChunk, StreamsChunk(*native));
    }
    if (file.native && file.form == ArchiveForm::kModel)
    {
        const Model model = ModelOf(file);
        write_chunk(kSoundsChunk, SoundsChunk(model));
        write_chunk(kItemsChunk, ItemsChunk(model));
    }
    if (!file.banks.empty())
    {
        write_chunk(kBanksChunk, BanksChunk(file));
    }
    if (!file.waves.empty())
    {
        write_chunk(kWavesChunk, WavesChunk(file));
    }
    for (const OtherChunk& chunk : file.other_chunks)
    {
        out.Line(0, "");
        out.Line(0, "chunk " + NameText(chunk.id) + " " + std::to_string(chunk.bytes.size()));
        out.Bytes(1, chunk.bytes);
    }

    return out.Take();
}

File ReadText(std::string_view text)
{
    Lines lines(WithoutByteOrderMark(text));
    File file;
    std::vector<Sequence> sequences;
    std::optional<NativeArchive> native;
    bool engine = false;
    bool header = false;
    bool group_files = false;
    bool streams = false;
    bool banks = false;
    bool waves = false;
    std::optional<std::vector<uint8_t>> sounds_chunk;
    std::optional<std::vector<uint8_t>> items_chunk;
    std::size_t model_line = 0;
    bool scene_open = false;
    std::array<bool, kAuxBuses> volume_given{};
    EditorView view;
    std::set<std::string> view_given;
    std::size_t view_line = 0;
    TimelineLines timeline_lines;
    Section section = Section::kHeader;
    std::optional<SequenceDraft> draft;
    std::vector<std::size_t> choices_lines;
    std::vector<std::vector<std::size_t>> item_lines;
    std::vector<ntdlib::sar::Piece> pieces;
    std::vector<FieldDraft> fields;
    std::vector<std::map<std::string, uint32_t>> locals;
    const auto finish = [&]
    {
        if (draft)
        {
            FinishSequence(*draft);
            sequences.push_back(std::move(draft->sequence));
            locals.push_back(std::move(draft->locals));
            choices_lines.push_back(draft->choices_line);
            item_lines.push_back(std::move(draft->lines));
            draft.reset();
        }
    };

    while (!lines.AtEnd())
    {
        const auto [line, number] = lines.Next();
        const std::vector<Token> tokens = Tokenize(line, number);
        if (tokens.empty())
        {
            continue;
        }

        // The first line names the format and its version.
        if (!header)
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
                Fail(number, "a later version of Hokuto made the file, with version " + tokens[1].text +
                                 " of the format. This version reads version " + std::to_string(kMajorVersion));
            }

            header = true;
            continue;
        }

        // A label, in a sequence.
        if (tokens.size() == 2 && tokens[1].Is(":"))
        {
            if (!draft)
            {
                Fail(number, "a label has to be in a sequence");
            }

            const auto item = static_cast<uint32_t>(draft->sequence.items.size());
            const bool local = !tokens[0].quoted && tokens[0].text.starts_with(".");
            std::map<std::string, uint32_t>& names = local ? draft->locals : draft->labels;
            if (!names.emplace(tokens[0].text, item).second)
            {
                Fail(number, "the sequence already has a label named " + tokens[0].text);
            }
            continue;
        }

        // A scene's values follow its line, before the sequences.
        const Token& word = tokens[0];
        if (section == Section::kHeader && (word.Is("variable") || word.Is("at")))
        {
            if (!scene_open)
            {
                Fail(number, word.text + " belongs to a scene, after the scene's line");
            }

            ParseSceneValue(tokens, number, file.scenes.back());
            continue;
        }

        scene_open = false;
        if (word.Is("engine") || word.Is("mixing"))
        {
            if (section != Section::kHeader || tokens.size() != 2)
            {
                Fail(number, word.text + " comes once, before the sequences, with a name");
            }

            if (word.Is("engine"))
            {
                const std::optional<Engine> named = EngineNamed(tokens[1].text);
                if (!named)
                {
                    Fail(number, "Hokuto has no engine named " + tokens[1].text);
                }

                file.engine = *named;
                engine = true;
            }
            else
            {
                const std::optional<Mixing> named = MixingNamed(tokens[1].text);
                if (!named)
                {
                    Fail(number, "Hokuto has no mixing mode named " + tokens[1].text);
                }

                file.mixing = *named;
            }
        }
        else if (word.Is("sound_mixing"))
        {
            if (section != Section::kHeader || tokens.size() != 3)
            {
                Fail(number,
                     "a sound's mixing mode is sound_mixing, the sound's number and the mode, before the "
                     "sequences");
            }

            const auto sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
            const std::optional<Mixing> named = MixingNamed(tokens[2].text);
            if (!named)
            {
                Fail(number, "Hokuto has no mixing mode named " + tokens[2].text);
            }
            if (!file.sound_mixing.emplace(sound, *named).second)
            {
                Fail(number, "sound " + tokens[1].text + "'s mixing mode comes twice");
            }
        }
        else if (word.Is("sound_engine"))
        {
            if (section != Section::kHeader || tokens.size() != 3)
            {
                Fail(number,
                     "a sound's engine is sound_engine, the sound's number and the engine, before the sequences");
            }

            const auto sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
            const std::optional<Engine> named = EngineNamed(tokens[2].text);
            if (!named)
            {
                Fail(number, "Hokuto has no engine named " + tokens[2].text);
            }
            if (!file.sound_engines.emplace(sound, *named).second)
            {
                Fail(number, "sound " + tokens[1].text + "'s engine comes twice");
            }
        }
        else if (word.Is("effect"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the effects come before the sequences");
            }

            auto [bus, effect] = ParseEffect(tokens, number);
            file.aux_effects[bus].push_back(std::move(effect));
        }
        else if (word.Is("bus_volume"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the buses' volumes come before the sequences");
            }

            const auto [bus, volume] = ParseBusVolume(tokens, number);
            if (std::exchange(volume_given[bus], true))
            {
                Fail(number, "the file gives bus " + tokens[1].text + "'s volume twice");
            }

            file.aux_volumes[bus] = volume;
        }
        else if (word.Is("tag") || word.Is("sound_tag"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the tags come before the sequences");
            }

            // The file's tag, or a sound's.
            const bool of_sound = word.Is("sound_tag");
            const std::size_t at = of_sound ? 2 : 1;
            if (tokens.size() != at + 2 || tokens[at].quoted)
            {
                Fail(number, of_sound ? "a sound's tag is sound_tag, the sound's number, the tag's key and its value"
                                      : "a tag is tag, its key and its value");
            }
            std::string whose;
            Tags* tags = &file.tags;
            if (of_sound)
            {
                const auto sound =
                    static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
                whose = "sound " + std::to_string(sound) + "'s ";
                tags = &file.sound_tags[sound];
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
        else if (word.Is("sound_length"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the sounds' lengths come before the sequences");
            }

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
            if (!file.lengths.emplace(sound, length).second)
            {
                Fail(number, whose + " come twice");
            }
        }
        else if (word.Is("scene"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the scenes come before the sequences");
            }
            if (tokens.size() != 3)
            {
                Fail(number, "a scene's line is scene, its sound's number and its name");
            }

            Scene& scene = file.scenes.emplace_back();
            scene.sound = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sound's number"));
            scene.name = tokens[2].text;
            scene_open = true;
        }
        else if (word.Is("track_colour"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the tracks' colours come before the sequences");
            }
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
            if (!file.track_colours.emplace(place, colour).second)
            {
                Fail(number, "track " + std::to_string(place.track) + " of sound " + std::to_string(place.sound) +
                                 " already has a colour");
            }
        }
        else if (word.Is("timeline"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the timelines come before the sequences");
            }

            ParseTimeline(tokens, number, file, timeline_lines);
        }
        else if (word.Is("view"))
        {
            if (section != Section::kHeader)
            {
                Fail(number, "the view comes before the sequences");
            }

            ParseView(tokens, number, view, view_given);
            view_line = view_line == 0 ? number : view_line;
        }
        else if (word.Is("sequence") && tokens.size() == 2)
        {
            if (section > Section::kSequence)
            {
                Fail(number, "the sequences come before the native archive and the chunks");
            }

            finish();
            const uint64_t index = ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sequence's number");
            if (index != sequences.size())
            {
                Fail(number, "the sequences are numbered from 0 in order. This one has to be " +
                                 std::to_string(sequences.size()));
            }

            section = Section::kSequence;
            draft.emplace();
        }
        else if (word.Is("labels") && draft)
        {
            if (!draft->order.empty())
            {
                Fail(number, "a sequence has one labels line");
            }

            draft->order_line = number;
            for (std::size_t t = 1; t < tokens.size(); ++t)
            {
                draft->order.push_back(tokens[t].text);
            }
        }
        else if (word.Is("native") && tokens.size() == 2)
        {
            if (section > Section::kSequence)
            {
                Fail(number, "the native archive comes once, before the chunks");
            }

            finish();
            const std::optional<ntdlib::sar::Flavor> flavor = FlavorNamed(tokens[1].text);
            const bool wii = tokens[1].Is(kWiiArchiveName);
            if (!flavor && !wii)
            {
                Fail(number, "Hokuto has no native archive named " + tokens[1].text);
            }

            section = Section::kNative;
            native = NativeArchive{.flavor = flavor.value_or(ntdlib::sar::Flavor::kNx), .wii = wii};
        }
        else if (word.Is("bytes") && section == Section::kNative && tokens.size() == 2)
        {
            const uint64_t size = ParseInteger(tokens[1], UINT64_MAX >> 1, number, "a size");
            pieces.emplace_back(lines.Bytes(size, number));
        }
        else if (word.Is("sequence_file") && section == Section::kNative && tokens.size() == 4 &&
                 tokens[2].Is("version"))
        {
            ntdlib::sar::SequencePiece piece;
            piece.sequence = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a sequence"));
            piece.version = static_cast<uint32_t>(ParseInteger(tokens[3], 0xFFFFFFFF, number, "a version"));
            if (piece.sequence >= sequences.size())
            {
                Fail(number, "the file has no sequence " + tokens[1].text);
            }

            pieces.emplace_back(piece);
        }
        else if (word.Is("padding") && section == Section::kNative && tokens.size() == 2)
        {
            const auto alignment = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "an alignment"));
            if (alignment == 0)
            {
                Fail(number, "padding aligns to a multiple of at least 1 byte");
            }

            pieces.emplace_back(ntdlib::sar::Padding{alignment});
        }
        else if (word.Is("field") && section == Section::kNative)
        {
            fields.push_back(ParseField(tokens, number));
        }
        else if (word.Is("tables") && section == Section::kNative && tokens.size() == 1)
        {
            if (native->tables)
            {
                Fail(number, "the native archive has one tables line");
            }

            native->tables.emplace();
        }
        else if (IsTablesWord(word) && section == Section::kNative)
        {
            if (!native->tables)
            {
                Fail(number, word.text + " belongs to the tables, after the tables line");
            }

            ParseTablesLine(tokens, number, *native->tables, native->game_sizes);
        }
        else if (word.Is("build") && section == Section::kNative &&
                 (tokens.size() == 2 || (tokens.size() == 3 && tokens[2].Is("october_2006"))))
        {
            if (!native->wii)
            {
                Fail(number, "only a native archive of the Wii has a build of the Wii's sound library");
            }
            if (native->wii_build)
            {
                Fail(number, "the native archive has one build line");
            }
            if (!pieces.empty())
            {
                Fail(number, "the native archive's build comes before its pieces");
            }

            ntdlib::SndBuild build;
            build.date = static_cast<uint32_t>(ParseInteger(tokens[1], 0xFFFFFFFF, number, "a date"));
            build.october_2006 = tokens.size() == 3;
            native->wii_build = build;
        }
        else if (word.Is("game_size") && section == Section::kNative && tokens.size() == 2)
        {
            if (native->game_sizes)
            {
                Fail(number, "the native archive has one game_size line");
            }
            if (native->tables)
            {
                Fail(number, "the native archive's game_size comes before the tables");
            }

            native->game_sizes.emplace().archive = ParseInteger(tokens[1], UINT64_MAX, number, "a size");
        }
        else if (word.Is("chunk") && tokens.size() == 3)
        {
            finish();
            section = Section::kChunks;
            const uint64_t size = ParseInteger(tokens[2], UINT64_MAX >> 1, number, "a size");
            if (tokens[1].text.size() != 4)
            {
                Fail(number, "a chunk's ID has four characters");
            }

            std::vector<uint8_t> bytes = lines.Bytes(size, number);
            if (tokens[1].text == kSoundsChunk || tokens[1].text == kItemsChunk)
            {
                // The model's chunks hold the archive of a file without a native archive.
                std::optional<std::vector<uint8_t>>& chunk =
                    tokens[1].text == kSoundsChunk ? sounds_chunk : items_chunk;
                if (native || chunk)
                {
                    Fail(number, "the " + tokens[1].text + " chunk comes once, in a file without a native archive");
                }

                chunk = std::move(bytes);
                model_line = model_line == 0 ? number : model_line;
                continue;
            }
            if (tokens[1].text == kStreamsChunk)
            {
                // The streams belong to the native archive, after its group files.
                if (!native || streams)
                {
                    Fail(number, "the STRM chunk comes once, after the native archive");
                }
                try
                {
                    native->streams = ReadStreams(bytes);
                }
                catch (const xsflib::FormatError& error)
                {
                    throw xsflib::FormatError("Line " + std::to_string(number) + ": " + error.what());
                }

                streams = true;
                continue;
            }
            if (tokens[1].text == kBanksChunk || tokens[1].text == kWavesChunk)
            {
                // The models of the edited banks and waves come once each, after the archive.
                bool& read = tokens[1].text == kBanksChunk ? banks : waves;
                if ((!native && !items_chunk) || read)
                {
                    Fail(number, "the " + tokens[1].text + " chunk comes once, after the archive");
                }
                try
                {
                    if (tokens[1].text == kBanksChunk)
                    {
                        file.banks = ReadBanks(bytes);
                    }
                    else
                    {
                        file.waves = ReadWaves(bytes);
                    }
                }
                catch (const xsflib::FormatError& error)
                {
                    throw xsflib::FormatError("Line " + std::to_string(number) + ": " + error.what());
                }

                read = true;
                continue;
            }
            if (tokens[1].text != kGroupFilesChunk)
            {
                file.other_chunks.push_back({tokens[1].text, std::move(bytes)});
                continue;
            }

            // The group files belong to the native archive.
            if (!native || group_files || streams)
            {
                Fail(number, "the GRPF chunk comes once, after the native archive and before its streams");
            }
            try
            {
                native->group_files = ReadGroupFiles(bytes, sequences);
            }
            catch (const xsflib::FormatError& error)
            {
                const std::string message = error.what();
                if (message.find("ends early") != std::string::npos)
                {
                    Fail(number, "the GRPF chunk ends early");
                }
                throw xsflib::FormatError("Line " + std::to_string(number) + ": " + message);
            }

            group_files = true;
        }
        else if (draft)
        {
            const auto at = static_cast<uint32_t>(draft->sequence.items.size());
            draft->sequence.items.push_back(ParseCommand(tokens, number, at, draft->references));
            draft->lines.push_back(number);
            if (draft->choices_line == 0 && HasChoices(draft->sequence.items.back()))
            {
                draft->choices_line = number;
            }
        }
        else
        {
            Fail(number, "Hokuto doesn't know " + word.text + " here");
        }
    }
    finish();

    if (!header)
    {
        Fail(1, "the text is empty");
    }
    if (!engine)
    {
        Fail(1, "the file names no engine");
    }

    // A view gives each part that a writer always writes, and holds what the program can show.
    if (view_line != 0)
    {
        for (const std::string_view part : {"sound", "position", "cycle", "zoom", "scroll", "key_zoom", "key_scroll"})
        {
            if (!view_given.contains(std::string(part)))
            {
                Fail(view_line, "the view gives no " + std::string(part));
            }
        }
        if (const std::optional<std::string> problem = ViewProblem(view))
        {
            Fail(view_line, "the view " + *problem);
        }

        // A view without a window in front puts the List Editor in front. Hokuto opened such a file that way.
        if (!view_given.contains("front"))
        {
            view.front = ViewFront::kListEditor;
        }
        file.view = view;
    }

    // The targets lie within their sequences.
    for (std::size_t s = 0; s < sequences.size(); ++s)
    {
        const std::vector<Command>& items = sequences[s].items;
        for (const Command& item : items)
        {
            if (!item.target_outside && item.target > items.size())
            {
                Fail(1, "a command of sequence " + std::to_string(s) + " goes past its end");
            }
        }
    }

    // The native archive takes its pieces.
    if (native)
    {
        native->pieces = std::move(pieces);
    }

    // The fields' places at the archive's end follow the last piece, and labels give items their numbers.
    for (FieldDraft& field_draft : fields)
    {
        ntdlib::sar::Field& field = field_draft.field;
        for (ntdlib::sar::Place* place : {&field.at, &field.from, &field.to})
        {
            place->piece = place->piece == kEndPiece ? static_cast<uint32_t>(native->pieces->size()) : place->piece;
        }
        if (field_draft.from_label)
        {
            field.from.offset =
                ItemOfLabel(*native, sequences, locals, field.from, *field_draft.from_label, field_draft.line);
        }
        if (field_draft.to_label)
        {
            field.to.offset =
                ItemOfLabel(*native, sequences, locals, field.to, *field_draft.to_label, field_draft.line);
        }
        if (const std::optional<std::string> problem = FieldProblem(*native, sequences, field))
        {
            Fail(field_draft.line, "the field " + *problem);
        }

        native->fields.push_back(field);
    }

    // The model of a file in the model form.
    std::optional<Model> model;
    if (sounds_chunk || items_chunk)
    {
        if (!sounds_chunk || !items_chunk)
        {
            Fail(model_line, "a file in the model form holds both the SNDS and the ITMS chunk");
        }

        try
        {
            model = ReadModel(*sounds_chunk, *items_chunk);
        }
        catch (const xsflib::FormatError& error)
        {
            throw xsflib::FormatError("Line " + std::to_string(model_line) + ": " + error.what());
        }
    }

    // Each command's values have to be values that the console's bytes hold.
    if (native || model)
    {
        const ntdlib::nwsnd::Grammar grammar = native ? GrammarOf(*native) : GrammarOf(*model);
        for (std::size_t s = 0; s < sequences.size(); ++s)
        {
            for (std::size_t i = 0; i < sequences[s].items.size(); ++i)
            {
                if (const std::optional<std::string> problem = CommandProblem(grammar, sequences[s].items[i]))
                {
                    Fail(item_lines[s][i], *problem);
                }
            }
        }
    }

    // The file shares its parts once they're whole. The model builds the native archive from the sequences.
    file.sequences = ShareSequences(std::move(sequences));
    if (native)
    {
        file.native = std::move(*native);
    }
    if (model)
    {
        try
        {
            file = WithModel(std::move(file), *model);
        }
        catch (const xsflib::FormatError& error)
        {
            throw xsflib::FormatError("Line " + std::to_string(model_line) + ": " + error.what());
        }
    }

    // Each timeline names a sequence of the file.
    for (const auto& [sequence, line] : timeline_lines.bars)
    {
        if (sequence >= file.sequences.size())
        {
            Fail(line, "the file has no sequence " + std::to_string(sequence));
        }
    }
    if (const std::optional<std::string> problem = TimelinesProblem(file))
    {
        Fail(timeline_lines.bars.empty() ? timeline_lines.none : timeline_lines.bars.begin()->second,
             "the timelines " + *problem);
    }

    // The encoder's choices belong to the sequence file of their sequence in the native archive or its group files.
    if (const std::optional<std::size_t> sequence = UnfiledChoices(file))
    {
        const std::string needed =
            "a sequence_file of sequence " + std::to_string(*sequence) + " in the native archive or its group files";
        Fail(choices_lines[*sequence],
             "[value_bytes], [unused_time], [unused_random] and [unused_variable] require " + needed);
    }

    return file;
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
