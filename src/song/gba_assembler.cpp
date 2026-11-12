// SPDX-License-Identifier: MIT

// The assembler that reads GBA songs and voice groups from assembly (song/gba_assembly.h).

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <utility>

#include "ntdlib/mp2k/commands.h"
#include "ntdlib/mp2k/driver.h"
#include "ntdlib/mp2k/song.h"

#include "song/gba_assembly.h"
#include "song/gba_assembly_parts.h"

namespace hokuto::song
{
namespace
{

namespace mp2k = ntdlib::mp2k;

using gba_assembly_parts::kCentre;
using gba_assembly_parts::kCommandNames;
using gba_assembly_parts::KeyName;
using gba_assembly_parts::kHighestVolume;
using gba_assembly_parts::kKeys;
using gba_assembly_parts::kLengthCodes;
using gba_assembly_parts::kMemoryOperations;
using gba_assembly_parts::kModTargets;
using gba_assembly_parts::kReverbSet;
using gba_assembly_parts::kVoiceBytes;
using gba_assembly_parts::ReadWord;
using gba_assembly_parts::ThreeDigits;
using gba_assembly_parts::TwoDigits;

// The file's parts.
struct Line
{
    int number = 0;
    std::string label;
    std::string directive;
    std::string arguments;
};

std::string Trimmed(std::string_view text)
{
    const auto space = [](char c)
    {
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!text.empty() && space(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back()))
    {
        text.remove_suffix(1);
    }

    return std::string(text);
}

[[noreturn]] void Refuse(int line, const std::string& what)
{
    throw std::invalid_argument("Line " + std::to_string(line) + " of the file: " + what);
}

bool IsSymbolStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '$';
}

bool IsSymbolPart(char c)
{
    return IsSymbolStart(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

// The definitions that files take from MPlayDef.s.
std::map<std::string, int64_t> DefinedSymbols()
{
    std::map<std::string, int64_t> symbols;
    for (const auto& [code, name] : kCommandNames)
    {
        symbols[name] = code;
    }

    for (int code = 0; code < kLengthCodes; code++)
    {
        symbols["W" + TwoDigits(mp2k::ClockLength(code))] = mp2k::kCmdWait + code;
        if (code > 0)
        {
            symbols["N" + TwoDigits(mp2k::ClockLength(code))] = mp2k::kCmdTie + code;
        }
    }

    for (int key = 0; key < 128; key++)
    {
        symbols[KeyName(key)] = key;
        symbols["v" + ThreeDigits(key)] = key;
    }

    for (int gate = 1; gate <= 3; gate++)
    {
        symbols["gtp" + std::to_string(gate)] = gate;
    }

    for (std::size_t target = 0; target < kModTargets.size(); target++)
    {
        symbols[kModTargets[target]] = static_cast<int64_t>(target);
    }

    for (std::size_t operation = 0; operation < kMemoryOperations.size(); operation++)
    {
        symbols[kMemoryOperations[operation]] = static_cast<int64_t>(operation);
    }

    symbols["PAM"] = mp2k::kCmdPan;
    symbols["PORT"] = mp2k::kCmdPort;
    symbols["c_v"] = kCentre;
    symbols["mxv"] = kHighestVolume;
    symbols["reverb_set"] = kReverbSet;
    symbols["xIECV"] = mp2k::kXcmdEchoVol;
    symbols["xIECL"] = mp2k::kXcmdEchoLen;

    return symbols;
}

// Splits `text` into its labels and directives. `@` starts a comment, and `;` ends a statement.
std::vector<Line> ParsedLines(std::string_view text)
{
    std::vector<Line> lines;
    int number = 0;
    std::size_t at = 0;
    while (at <= text.size())
    {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        number++;
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        line = line.substr(0, std::min(line.find('@'), line.size()));
        std::size_t start = 0;
        while (start <= line.size())
        {
            const std::size_t stop = std::min(line.find(';', start), line.size());
            std::string statement = Trimmed(line.substr(start, stop - start));
            start = stop + 1;

            // A label comes before its colon, and a directive can follow it.
            Line parsed;
            parsed.number = number;
            const std::size_t colon = statement.find(':');
            if (colon != std::string::npos)
            {
                parsed.label = Trimmed(std::string_view(statement).substr(0, colon));
                const bool global = colon + 1 < statement.size() && statement[colon + 1] == ':';
                statement = Trimmed(std::string_view(statement).substr(colon + (global ? 2 : 1)));
                if (parsed.label.empty() || !std::ranges::all_of(parsed.label, IsSymbolPart))
                {
                    Refuse(number, "Can't read the label \"" + parsed.label + "\".");
                }
            }

            const std::size_t space = statement.find_first_of(" \t");
            parsed.directive = statement.substr(0, space);
            parsed.arguments =
                space == std::string::npos ? std::string() : Trimmed(std::string_view(statement).substr(space));
            if (!parsed.label.empty() || !parsed.directive.empty())
            {
                lines.push_back(std::move(parsed));
            }
        }
    }

    return lines;
}

// The items of a list of arguments, split at its commas.
std::vector<std::string> SplitList(std::string_view text)
{
    std::vector<std::string> items;
    std::size_t at = 0;
    while (at <= text.size())
    {
        const std::size_t comma = std::min(text.find(',', at), text.size());
        std::string item = Trimmed(text.substr(at, comma - at));
        if (!item.empty())
        {
            items.push_back(std::move(item));
        }

        at = comma + 1;
    }

    return items;
}

// The arithmetic of a directive's arguments. Unknown() returns a name in them that the file doesn't define.
class Expression
{
public:
    Expression(std::string_view text, const std::map<std::string, std::string>& assignments,
               const std::map<std::string, int64_t>& values, int line, int depth = 0)
        : text_(text), assignments_(assignments), values_(values), line_(line), depth_(depth)
    {
    }

    int64_t Value()
    {
        const int64_t value = Sum();
        Space();
        if (at_ != text_.size())
        {
            Refuse(line_, "Can't read \"" + std::string(text_) + "\".");
        }

        return value;
    }

    const std::string& Unknown() const
    {
        return unknown_;
    }

private:
    void Space()
    {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])) != 0)
        {
            at_++;
        }
    }

    bool Take(char c)
    {
        Space();
        if (at_ < text_.size() && text_[at_] == c)
        {
            at_++;
            return true;
        }

        return false;
    }

    int64_t Sum()
    {
        int64_t value = Product();
        for (;;)
        {
            if (Take('+'))
            {
                value += Product();
            }
            else if (Take('-'))
            {
                value -= Product();
            }
            else
            {
                return value;
            }
        }
    }

    int64_t Product()
    {
        int64_t value = Unary();
        for (;;)
        {
            if (Take('*'))
            {
                value *= Unary();
            }
            else if (Take('/'))
            {
                const int64_t divisor = Unary();
                if (divisor == 0)
                {
                    Refuse(line_, "\"" + std::string(text_) + "\" divides by 0.");
                }

                value /= divisor;
            }
            else
            {
                return value;
            }
        }
    }

    int64_t Unary()
    {
        if (Take('-'))
        {
            return -Unary();
        }
        if (Take('+'))
        {
            return Unary();
        }

        if (Take('('))
        {
            const int64_t value = Sum();
            if (!Take(')'))
            {
                Refuse(line_, "\"" + std::string(text_) + "\" is missing a closing parenthesis.");
            }

            return value;
        }

        Space();
        if (at_ == text_.size())
        {
            Refuse(line_, "\"" + std::string(text_) + "\" ends too early.");
        }

        if (std::isdigit(static_cast<unsigned char>(text_[at_])) != 0)
        {
            return Number();
        }

        if (!IsSymbolStart(text_[at_]))
        {
            Refuse(line_, "Can't read \"" + std::string(text_) + "\".");
        }

        const std::size_t start = at_;
        while (at_ < text_.size() && IsSymbolPart(text_[at_]))
        {
            at_++;
        }

        return Symbol(std::string(text_.substr(start, at_ - start)));
    }

    // GNU as reads 0x or 0X and hex digits as hexadecimal, a 0 and more digits as octal, and the rest as decimal.
    int64_t Number()
    {
        const bool hex =
            text_.size() - at_ > 2 && text_[at_] == '0' && (text_[at_ + 1] == 'x' || text_[at_ + 1] == 'X');
        const bool octal = !hex && text_.size() - at_ > 1 && text_[at_] == '0' &&
                           std::isdigit(static_cast<unsigned char>(text_[at_ + 1])) != 0;
        at_ += hex ? 2 : 0;
        int64_t value = 0;
        int base = 10;
        if (hex)
        {
            base = 16;
        }
        else if (octal)
        {
            base = 8;
        }

        const std::from_chars_result read =
            std::from_chars(text_.data() + at_, text_.data() + text_.size(), value, base);
        const bool past_octal = octal && read.ptr != text_.data() + text_.size() &&
                                std::isdigit(static_cast<unsigned char>(*read.ptr)) != 0;
        if (read.ec != std::errc() || past_octal)
        {
            Refuse(line_, "Can't read the number in \"" + std::string(text_) + "\".");
        }

        at_ = static_cast<std::size_t>(read.ptr - text_.data());

        return value;
    }

    int64_t Symbol(const std::string& name)
    {
        if (const auto value = values_.find(name); value != values_.end())
        {
            return value->second;
        }

        const auto assigned = assignments_.find(name);
        if (assigned == assignments_.end())
        {
            unknown_ = name;
            return 0;
        }

        if (depth_ > 32)
        {
            Refuse(line_, "\"" + name + "\" is defined in terms of itself.");
        }

        Expression inner(assigned->second, assignments_, values_, line_, depth_ + 1);
        const int64_t value = inner.Value();
        if (unknown_.empty())
        {
            unknown_ = inner.Unknown();
        }

        return value;
    }

    std::string_view text_;
    const std::map<std::string, std::string>& assignments_;
    const std::map<std::string, int64_t>& values_;
    int line_;
    int depth_;
    std::size_t at_ = 0;
    std::string unknown_;
};

// The directives that take no room and give no values.
bool IsIgnored(std::string_view directive)
{
    constexpr std::array<std::string_view, 7> kIgnored = {".section", ".text", ".data",  ".include",
                                                          ".type",    ".size", ".syntax"};
    return std::ranges::find(kIgnored, directive) != kIgnored.end();
}

// The size in bytes of each value of a directive that lays values out, or 0 for another directive.
std::size_t ValueSize(std::string_view directive)
{
    if (directive == ".byte")
    {
        return 1;
    }
    if (directive == ".2byte" || directive == ".hword" || directive == ".short")
    {
        return 2;
    }
    if (directive == ".word" || directive == ".4byte" || directive == ".long" || directive == ".int")
    {
        return 4;
    }

    return 0;
}

// The bytes that `.align` or `.balign` puts after `size` bytes.
std::size_t PadOf(const Line& line, std::size_t size)
{
    int64_t power = 0;
    const std::string first = SplitList(line.arguments).empty() ? std::string() : SplitList(line.arguments).front();
    const std::from_chars_result read = std::from_chars(first.data(), first.data() + first.size(), power);
    if (read.ec != std::errc() || power < 0 || (line.directive == ".align" && power > 16) || power > 65'536)
    {
        Refuse(line.number, "Can't read the alignment \"" + line.arguments + "\".");
    }

    const std::size_t step = line.directive == ".align" ? std::size_t{1} << power : static_cast<std::size_t>(power);
    return step == 0 ? 0 : (step - size % step) % step;
}

} // namespace

namespace gba_assembly_parts
{

uint32_t ReadWord(std::span<const uint8_t> bytes, std::size_t at)
{
    return uint32_t{bytes[at]} | uint32_t{bytes[at + 1]} << 8 | uint32_t{bytes[at + 2]} << 16 |
           uint32_t{bytes[at + 3]} << 24;
}

} // namespace gba_assembly_parts

namespace
{

// A file's bytes and its labels.
struct LaidOut
{
    std::vector<uint8_t> bytes;
    std::map<std::string, std::size_t> offsets; // each label's offset in `bytes`
    std::string main;                           // the label that the file's first `.global` names, or else its last
    std::size_t main_offset = 0;
};

// What the first pass of LayOut finds besides the labels: the assignments, the name that the first `.global` names,
// the first and the last label, and the size of the bytes.
struct Placement
{
    std::map<std::string, std::string> assignments;
    std::string global;
    std::string first_label;
    std::string last_label;
    std::size_t size = 0;
};

// The first pass of LayOut: the offset of each label in `laid`, and its address in `values`. Each directive's size
// doesn't depend on its values.
Placement PlaceLabels(const std::vector<Line>& lines, uint32_t base, const std::map<std::string, uint32_t>& placed,
                      LaidOut& laid, std::map<std::string, int64_t>& values)
{
    Placement placement;
    for (const Line& line : lines)
    {
        if (!line.label.empty())
        {
            laid.offsets[line.label] = placement.size;
            const auto elsewhere = placed.find(line.label);
            values[line.label] = elsewhere != placed.end() ? int64_t{elsewhere->second}
                                                           : int64_t{base} + static_cast<int64_t>(placement.size);
            placement.first_label = placement.first_label.empty() ? line.label : placement.first_label;
            placement.last_label = line.label;
        }

        if (line.directive == ".end")
        {
            break;
        }

        if (line.directive == ".equ" || line.directive == ".set" || line.directive == ".equiv")
        {
            const std::size_t comma = line.arguments.find(',');
            if (comma == std::string::npos)
            {
                Refuse(line.number, "\"" + line.directive + " " + line.arguments + "\" has no value.");
            }

            std::string name = Trimmed(std::string_view(line.arguments).substr(0, comma));
            placement.assignments[name] = Trimmed(std::string_view(line.arguments).substr(comma + 1));
            values.erase(name);
        }
        else if (line.directive == ".global" || line.directive == ".globl")
        {
            placement.global = placement.global.empty() ? line.arguments : placement.global;
        }
        else if (line.directive == ".align" || line.directive == ".balign")
        {
            placement.size += PadOf(line, placement.size);
        }
        else if (const std::size_t each = ValueSize(line.directive); each > 0)
        {
            placement.size += each * SplitList(line.arguments).size();
        }
        else if (!line.directive.empty() && !IsIgnored(line.directive))
        {
            Refuse(line.number, "Hokuto doesn't support the directive \"" + line.directive + "\".");
        }
    }

    return placement;
}

// The second pass of LayOut: the bytes of `lines`, with `unknown` for the names that the file doesn't define.
void AssembleLines(
    const std::vector<Line>& lines, const std::map<std::string, std::string>& assignments,
    const std::map<std::string, int64_t>& values,
    const std::function<bool(const std::string&, const LaidOut&, std::size_t, std::size_t, int64_t&)>& unknown,
    LaidOut& laid)
{
    for (const Line& line : lines)
    {
        if (line.directive == ".end")
        {
            break;
        }

        if (line.directive == ".align" || line.directive == ".balign")
        {
            laid.bytes.insert(laid.bytes.end(), PadOf(line, laid.bytes.size()), 0);
            continue;
        }

        const std::size_t each = ValueSize(line.directive);
        if (each == 0)
        {
            continue;
        }

        for (const std::string& item : SplitList(line.arguments))
        {
            Expression expression(item, assignments, values, line.number);
            int64_t value = expression.Value();
            if (!expression.Unknown().empty() && !unknown(expression.Unknown(), laid, laid.bytes.size(), each, value))
            {
                Refuse(line.number, "Unknown symbol \"" + expression.Unknown() + "\".");
            }

            for (std::size_t b = 0; b < each; b++)
            {
                laid.bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(value) >> (8 * b)));
            }
        }
    }
}

// Lays the file `text` out, with its first byte at address `base`. A first pass finds the labels and the assignments:
// each directive's size doesn't depend on its values. `before` is then called with them. In the second pass, `unknown`
// sets the value of a name that the file doesn't define, from the offset and the size of the value that names it.
// `unknown` returns false for a name that it doesn't resolve. A label in `placed` gets its address from `placed`. The
// file's main label is the one that its first `.global` names. Without one, it is the first label when `first_is_main`
// is true, or else the last.
LaidOut LayOut(
    std::string_view text, uint32_t base, const std::map<std::string, uint32_t>& placed, bool first_is_main,
    const std::function<void(const LaidOut&)>& before,
    const std::function<bool(const std::string&, const LaidOut&, std::size_t, std::size_t, int64_t&)>& unknown)
{
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3);
    }

    const std::vector<Line> lines = ParsedLines(text);

    std::map<std::string, int64_t> values = DefinedSymbols();
    LaidOut laid;
    const Placement placement = PlaceLabels(lines, base, placed, laid, values);

    laid.main = laid.offsets.contains(placement.global) ? placement.global
                : first_is_main                         ? placement.first_label
                                                        : placement.last_label;
    const auto found_main = laid.offsets.find(laid.main);
    if (found_main == laid.offsets.end())
    {
        throw std::invalid_argument("The file contains no song header or voice group. It has no labels.");
    }

    laid.main_offset = found_main->second;
    if (before)
    {
        before(laid);
    }

    laid.bytes.reserve(placement.size);
    AssembleLines(lines, placement.assignments, values, unknown, laid);

    return laid;
}

} // namespace

GbaSongSource ReadGbaAssembly(std::string_view text, uint32_t base)
{
    std::size_t header = 0;
    GbaSongSource source;
    const std::function<bool(const std::string&, const LaidOut&, std::size_t, std::size_t, int64_t&)> unknown =
        [&](const std::string& name, const LaidOut&, std::size_t at, std::size_t each, int64_t& value)
    {
        // Only the header's voice group can name something that the file doesn't define.
        if (at != header + 4 || each != 4)
        {
            return false;
        }

        source.voices_symbol = name;
        value = 0;
        return true;
    };
    const LaidOut laid =
        LayOut(text, base, {}, false, [&](const LaidOut& so_far) { header = so_far.main_offset; }, unknown);
    const std::vector<uint8_t>& bytes = laid.bytes;
    const std::string& header_label = laid.main;
    source.name = header_label;

    if (bytes.size() < header + 8)
    {
        throw std::invalid_argument("The header " + header_label + " ends too early.");
    }

    const std::size_t tracks = bytes[header];
    if (tracks > static_cast<std::size_t>(ntdlib::mp2k::kMaxTracks))
    {
        throw std::invalid_argument("The header " + header_label + " lists " + std::to_string(tracks) +
                                    " tracks. A song plays at most " + std::to_string(ntdlib::mp2k::kMaxTracks) + ".");
    }
    if (bytes.size() < header + 8 + 4 * tracks)
    {
        throw std::invalid_argument("The header " + header_label + " ends before the last of its " +
                                    std::to_string(tracks) + " tracks.");
    }

    source.blocks = bytes[header + 1];
    source.priority = bytes[header + 2];
    source.reverb = bytes[header + 3];
    if (source.voices_symbol.empty())
    {
        source.voices = ReadWord(bytes, header + 4);
    }

    uint32_t lowest = base + static_cast<uint32_t>(header);
    for (std::size_t t = 0; t < tracks; t++)
    {
        const uint32_t entry = ReadWord(bytes, header + 8 + 4 * t);
        if (entry < base || entry >= base + header)
        {
            throw std::invalid_argument("Track " + std::to_string(t + 1) + " of the header " + header_label +
                                        " doesn't start before the header.");
        }

        source.entries.push_back(entry);
        lowest = std::min(lowest, entry);
    }

    source.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(lowest - base),
                        bytes.begin() + static_cast<std::ptrdiff_t>(header));
    source.base = lowest;

    return source;
}

std::optional<uint32_t> GbaAddressOfName(std::string_view name)
{
    const std::size_t underscore = name.rfind('_');
    if (underscore == std::string_view::npos || name.size() - underscore - 1 != 8)
    {
        return std::nullopt;
    }

    uint32_t address = 0;
    const char* const end = name.data() + name.size();
    const std::from_chars_result read = std::from_chars(name.data() + underscore + 1, end, address, 16);
    if (read.ec != std::errc() || read.ptr != end)
    {
        return std::nullopt;
    }

    return address;
}

GbaVoiceGroupSource ReadGbaVoiceGroup(std::string_view text, const std::map<std::string, uint32_t>& placed)
{
    const auto unknown = [](const std::string& name, const LaidOut&, std::size_t, std::size_t, int64_t& value)
    {
        const std::optional<uint32_t> address = GbaAddressOfName(name);
        if (address)
        {
            value = *address;
        }

        return address.has_value();
    };
    LaidOut laid = LayOut(text, kGbaAssemblyBase, placed, true, {}, unknown);

    GbaVoiceGroupSource source;
    source.name = laid.main;
    source.table = laid.main_offset;

    // Each label's bytes run to the next label.
    std::vector<std::pair<std::size_t, std::string>> labels;
    for (const auto& [name, offset] : laid.offsets)
    {
        labels.emplace_back(offset, name);
    }

    std::ranges::sort(labels);
    std::size_t table_end = laid.bytes.size();
    for (std::size_t i = 0; i < labels.size(); i++)
    {
        const std::size_t end = i + 1 < labels.size() ? labels[i + 1].first : laid.bytes.size();
        if (labels[i].second == laid.main)
        {
            table_end = end;
        }
        else
        {
            source.parts.push_back({labels[i].second, labels[i].first, end - labels[i].first});
        }
    }

    const std::size_t size = table_end - laid.main_offset;
    if (size == 0 || size % kVoiceBytes != 0 || size / kVoiceBytes > kKeys)
    {
        throw std::invalid_argument("The voice group table " + laid.main +
                                    " doesn't contain a whole number of voices, up to 128.");
    }

    source.voices = size / kVoiceBytes;
    source.bytes = std::move(laid.bytes);

    return source;
}

std::string GbaAssemblyDefinitions()
{
    std::string out;
    for (const auto& [name, value] : DefinedSymbols())
    {
        out += "\t.equ\t" + name + ", " + std::to_string(value) + "\n";
    }

    return out;
}

} // namespace hokuto::song
