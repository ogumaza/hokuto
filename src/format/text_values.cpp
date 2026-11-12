// SPDX-License-Identifier: MIT

// Numbers, names and bytes in the text form and back, and the tokens of a line.

#include "format/text_parts.h"

namespace hokuto::format::text_parts
{

namespace
{

constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

// The kinds whose number follows their name: a track's, a bus's, an LFO's, a variable's or a register's.
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
    case Kind::kJumpIfEqual:
    case Kind::kJumpIfNotEqual:
    case Kind::kJumpIfGreater:
    case Kind::kJumpIfGreaterOrEqual:
    case Kind::kJumpIfLessOrEqual:
    case Kind::kJumpIfLess:
    case Kind::kRegister:
        return true;
    default:
        return false;
    }
}

// Whether a command of `kind` has a key: a note, and the end of a tie or of a held note (kNoteOff).
bool HasKey(Kind kind)
{
    return kind == Kind::kNote || kind == Kind::kNoteOff;
}

bool HasValue(Kind kind)
{
    return ntdlib::commands::UnitOf(kind).has_value();
}

namespace
{

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

} // namespace

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

std::string Hex(uint32_t value)
{
    return xsflib::Hex(value, 0, true);
}

namespace
{

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

} // namespace

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

[[noreturn]] void Fail(std::size_t line, const std::string& message)
{
    // ntdlib's DSE encoder ends its reasons with a full stop (DseProblem).
    const char* const stop = message.ends_with('.') ? "" : ".";
    throw xsflib::FormatError("Line " + std::to_string(line) + ": " + message + stop);
}

namespace
{

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

// The quoted name that starts at `i`. Moves `i` past its closing quote.
Token QuotedToken(std::string_view line, std::size_t& i, std::size_t number)
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
        else if (i + 2 < line.size() && line[i] == 'x' && HexDigit(line[i + 1]) >= 0 && HexDigit(line[i + 2]) >= 0)
        {
            token.text += static_cast<char>(HexDigit(line[i + 1]) * 16 + HexDigit(line[i + 2]));
            i += 3;
        }
        else
        {
            Fail(number, "a name contains a backslash that isn't part of \\\", \\\\ or \\x and two hex digits");
        }
    }

    return token;
}

// The bare word that starts at `i`, through any parentheses. Moves `i` past it.
Token BareToken(std::string_view line, std::size_t& i, std::size_t number)
{
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

    return token;
}

} // namespace

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
            tokens.push_back(QuotedToken(line, i, number));
            continue;
        }

        tokens.push_back(BareToken(line, i, number));
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

namespace
{

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

} // namespace

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

// The bytes of a line of base64.
void DecodeBase64(std::string_view text, std::vector<uint8_t>& bytes, std::size_t line)
{
    if (text.size() % 4 != 0)
    {
        Fail(line, "a line of base64 has to have a multiple of 4 characters");
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
                Fail(line, "a line of base64 contains a character that isn't base64");
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

} // namespace hokuto::format::text_parts
