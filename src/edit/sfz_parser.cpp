// SPDX-License-Identifier: MIT

// The reading of an SFZ file for edit/sfz_imports.h: its lines, its sections, its #define and #include lines, and the
// opcodes that each region inherits from its sections. sfz_imports.cpp builds the instruments from the regions.

#include "edit/sfz_parser.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/files.h"

namespace hokuto::edit::sfz_parser
{
namespace
{

constexpr int kDeepestInclude = 16;

bool IsNameCharacter(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }

    return text;
}

// `text` without its block comments.
std::string WithoutBlockComments(std::string_view text)
{
    std::string kept;
    while (!text.empty())
    {
        const std::size_t start = text.find("/*");
        kept += text.substr(0, start);
        if (start == std::string_view::npos)
        {
            break;
        }

        const std::size_t end = text.find("*/", start + 2);
        if (end == std::string_view::npos)
        {
            break;
        }

        kept += ' ';
        text.remove_prefix(end + 2);
    }

    return kept;
}

// The opcode that a name stands for: SFZ's later versions name some opcodes otherwise.
std::string CanonicalName(const std::string& name)
{
    static const std::map<std::string, std::string, std::less<>> kAliases = {
        {"loopmode", "loop_mode"}, {"loopstart", "loop_start"}, {"loopend", "loop_end"}, {"polyphony_group", "group"},
        {"offby", "off_by"},       {"gain", "volume"},          {"pitch", "tune"},
    };
    const auto found = kAliases.find(name);

    return found != kAliases.end() ? found->second : name;
}

// Reads the regions of an SFZ file, with what its sections give them.
class Parser
{
public:
    Parser(std::filesystem::path folder, const SfzReader& read) : folder_(std::move(folder)), read_(read)
    {
    }

    void Read(const std::filesystem::path& path, int depth)
    {
        std::vector<uint8_t> bytes;
        if (depth > kDeepestInclude || !read_(path, bytes))
        {
            throw std::invalid_argument("Can't read " + xsflib::Utf8(path.filename()) + ".");
        }

        const std::string text =
            WithoutBlockComments(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        std::size_t at = 0;
        while (at < text.size())
        {
            std::size_t end = text.find('\n', at);
            end = end == std::string::npos ? text.size() : end;
            ReadLine(std::string_view(text).substr(at, end - at), depth);
            at = end + 1;
        }
    }

    std::vector<Opcodes> Regions()
    {
        Flush();

        return std::move(regions_);
    }

    const std::map<std::string, std::size_t>& OtherSections() const
    {
        return other_sections_;
    }

private:
    enum class Level
    {
        kControl,
        kGlobal,
        kMaster,
        kGroup,
        kRegion,
        kOther
    };

    void ReadLine(std::string_view line, int depth)
    {
        line = line.substr(0, line.find("//"));
        line = Trimmed(line);
        if (line.starts_with("#define"))
        {
            line = Trimmed(line.substr(7));
            const std::size_t space = line.find_first_of(" \t");
            if (space != std::string_view::npos)
            {
                defines_[std::string(line.substr(0, space))] = std::string(Trimmed(line.substr(space)));
            }

            return;
        }
        if (line.starts_with("#include"))
        {
            const std::size_t first = line.find('"');
            const std::size_t last = line.rfind('"');
            if (first != std::string_view::npos && last > first)
            {
                std::string name(line.substr(first + 1, last - first - 1));
                std::ranges::replace(name, '\\', '/');
                Read(folder_ / xsflib::PathFromUtf8(name), depth + 1);
            }

            return;
        }

        const std::string text = WithDefines(line);
        std::string_view rest = text;
        while (true)
        {
            rest = Trimmed(rest);
            if (rest.empty())
            {
                return;
            }

            if (rest.front() == '<')
            {
                const std::size_t close = rest.find('>');
                if (close == std::string_view::npos)
                {
                    return;
                }

                Header(Lowered(rest.substr(1, close - 1)));
                rest.remove_prefix(close + 1);
                continue;
            }

            // A value runs up to the next opcode or header: a sample's path can hold spaces.
            const std::size_t equals = rest.find('=');
            if (equals == std::string_view::npos)
            {
                return;
            }

            const std::string_view name = Trimmed(rest.substr(0, equals));
            std::size_t end = equals + 1;
            while (end < rest.size() && !StartsNextToken(rest, end))
            {
                end++;
            }

            Set(CanonicalName(Lowered(name)), std::string(Trimmed(rest.substr(equals + 1, end - equals - 1))));
            rest.remove_prefix(end);
        }
    }

    static bool StartsNextToken(std::string_view text, std::size_t at)
    {
        if (std::isspace(static_cast<unsigned char>(text[at])) == 0)
        {
            return text[at] == '<';
        }

        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0)
        {
            at++;
        }
        if (at < text.size() && text[at] == '<')
        {
            return true;
        }
        while (at < text.size() && IsNameCharacter(text[at]))
        {
            at++;
        }

        return at < text.size() && text[at] == '=';
    }

    // `line` with each variable's value. A longer name is tried first: $KICK2 isn't $KICK.
    std::string WithDefines(std::string_view line) const
    {
        std::string text(line);
        for (auto define = defines_.rbegin(); define != defines_.rend(); ++define)
        {
            std::size_t at = 0;
            while ((at = text.find(define->first, at)) != std::string::npos)
            {
                text.replace(at, define->first.size(), define->second);
                at += define->second.size();
            }
        }

        return text;
    }

    void Header(const std::string& name)
    {
        Flush();
        if (name == "region")
        {
            level_ = Level::kRegion;
            in_region_ = true;
            region_ = control_;
            for (const Opcodes* level : {&global_, &master_, &group_})
            {
                for (const auto& [opcode, value] : *level)
                {
                    region_.insert_or_assign(opcode, value);
                }
            }
        }
        else if (name == "group")
        {
            level_ = Level::kGroup;
            group_.clear();
        }
        else if (name == "master")
        {
            level_ = Level::kMaster;
            master_.clear();
            group_.clear();
        }
        else if (name == "global")
        {
            level_ = Level::kGlobal;
            global_.clear();
            master_.clear();
            group_.clear();
        }
        else if (name == "control")
        {
            level_ = Level::kControl;
        }
        else
        {
            level_ = Level::kOther;
            if (name != "curve")
            {
                ++other_sections_["<" + name + ">"];
            }
        }
    }

    void Flush()
    {
        if (in_region_)
        {
            regions_.push_back(std::move(region_));
            region_.clear();
            in_region_ = false;
        }
    }

    void Set(const std::string& name, std::string value)
    {
        switch (level_)
        {
        case Level::kControl:
            control_.insert_or_assign(name, std::move(value));
            break;
        case Level::kGlobal:
            global_.insert_or_assign(name, std::move(value));
            break;
        case Level::kMaster:
            master_.insert_or_assign(name, std::move(value));
            break;
        case Level::kGroup:
            group_.insert_or_assign(name, std::move(value));
            break;
        case Level::kRegion:
            region_.insert_or_assign(name, std::move(value));
            break;
        case Level::kOther:
            break;
        }
    }

    std::filesystem::path folder_;
    const SfzReader& read_;
    std::map<std::string, std::string> defines_;
    Level level_ = Level::kOther;
    bool in_region_ = false;
    Opcodes control_;
    Opcodes global_;
    Opcodes master_;
    Opcodes group_;
    Opcodes region_;
    std::vector<Opcodes> regions_;
    std::map<std::string, std::size_t> other_sections_;
};

} // namespace

std::string Lowered(std::string_view text)
{
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return lowered;
}

SfzRegions ReadSfzRegions(const std::filesystem::path& folder, const SfzReader& read, const std::filesystem::path& path)
{
    Parser parser(folder, read);
    parser.Read(path, 0);
    SfzRegions result;
    result.other_sections = parser.OtherSections();
    result.regions = parser.Regions();

    return result;
}

} // namespace hokuto::edit::sfz_parser
