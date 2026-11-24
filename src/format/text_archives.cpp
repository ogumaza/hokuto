// SPDX-License-Identifier: MIT

// The text form of a file with more than one archive (format/project.h). The lines of the project come first: the
// version, the engine `archives`, the mixing mode, the archive in front, the file's tags, the effects and the volumes
// of the buses, and the chunks that Hokuto doesn't read. A line `archive` then starts each archive, and the lines of
// the archive follow in the order of a file of one archive, without the version.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xsflib/binary.h"

#include "format/codes.h"
#include "format/file.h"
#include "format/project.h"
#include "format/text.h"
#include "format/text_parts.h"

namespace hokuto::format
{
namespace
{

// The lines of `text`, without the line breaks. A line keeps a carriage return before its line feed.
std::vector<std::string_view> LinesOf(std::string_view text)
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            if (start < text.size())
            {
                lines.push_back(text.substr(start));
            }
            break;
        }

        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }

    return lines;
}

// `line` without a carriage return at its end.
std::string_view Bare(std::string_view line)
{
    return !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line;
}

// The tokens of `line`, the line `number`. The reader of a file of one archive splits its lines into the same tokens. A
// comment and the spaces between words don't count.
std::vector<text_parts::Token> TokensOf(std::string_view line, std::size_t number)
{
    return text_parts::Tokenize(Bare(line), number);
}

// Whether `tokens` are the bare words `words`, in order.
bool AreWords(const std::vector<text_parts::Token>& tokens, std::initializer_list<std::string_view> words)
{
    if (tokens.size() != words.size())
    {
        return false;
    }

    std::size_t i = 0;
    for (const std::string_view word : words)
    {
        if (!tokens[i++].Is(word))
        {
            return false;
        }
    }

    return true;
}

// The lines `first` to `last` of `lines` as a text of their own. Line 1 keeps the version. Blank lines stand for the
// other lines: a message then names the line's number in the whole text.
std::string Section(const std::vector<std::string_view>& lines, std::size_t first, std::size_t last)
{
    std::string text(lines.front());
    text += '\n';
    for (std::size_t i = 1; i < last; ++i)
    {
        if (i >= first)
        {
            text += lines[i];
        }
        text += '\n';
    }

    return text;
}

// Throws xsflib::FormatError for line `number` with `message`.
[[noreturn]] void FailAt(std::size_t number, const std::string& message)
{
    throw xsflib::FormatError("Line " + std::to_string(number) + ": " + message + ".");
}

} // namespace

std::string WriteProjectText(const Project& project)
{
    if (const std::optional<std::string> problem = ProjectProblem(project))
    {
        throw std::invalid_argument("the project " + *problem);
    }
    if (project.archives.size() == 1)
    {
        return WriteText(project.archives.front());
    }

    // The lines of the project: a file of its values alone, with the engine of more than one archive and the archive
    // in front after the mixing mode.
    const File& first = project.archives.front();
    File values;
    values.engine = first.engine;
    values.mixing = first.mixing;
    values.aux_effects = first.aux_effects;
    values.aux_volumes = first.aux_volumes;
    values.tags = first.tags;
    values.other_chunks = project.other_chunks;
    const std::string head = WriteText(values);
    const std::vector<std::string_view> head_lines = LinesOf(head);
    std::string text;
    for (std::size_t i = 0; i < head_lines.size(); ++i)
    {
        text += i == 1 ? "engine " + std::string(kArchivesEngineName) : std::string(head_lines[i]);
        text += '\n';
        if (i == 2)
        {
            text += std::string(kFrontWord) + " " + std::to_string(project.front) + "\n";
        }
    }

    // Each archive, without its version line.
    for (const File& archive : project.archives)
    {
        const std::string body = WriteText(WithoutProjectValues(archive));
        text += "\n" + std::string(kArchiveLine) + "\n";
        text += body.substr(body.find('\n') + 1);
    }

    return text;
}

Project ReadTextProject(std::string_view text)
{
    // A file of one archive.
    const std::vector<std::string_view> lines = LinesOf(text);
    std::optional<std::size_t> engine;
    std::vector<std::size_t> archive_lines;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::vector<text_parts::Token> tokens = TokensOf(lines[i], i + 1);
        if (AreWords(tokens, {"engine", kArchivesEngineName}) && !engine && archive_lines.empty())
        {
            engine = i;
        }
        else if (AreWords(tokens, {kArchiveLine}))
        {
            archive_lines.push_back(i);
        }
    }

    if (!engine)
    {
        return ProjectOf(ReadText(text));
    }

    // Version 1 of the format has no file with more than one archive.
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    if (MajorVersionOf(bytes).value_or(0) < 2)
    {
        FailAt(1, "a file with more than one archive is of version 2.0 or later");
    }

    // The lines of the project, read as a file of their values alone. The archive in front has a line of its own.
    const std::size_t head_end = archive_lines.empty() ? lines.size() : archive_lines.front();
    std::vector<std::string_view> head(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(head_end));
    const std::string stand_in = "engine " + std::string(NameOf(Engine::kGba));
    head[*engine] = stand_in;
    Project project;
    std::optional<std::size_t> front;
    for (std::size_t i = 0; i < head.size(); ++i)
    {
        const std::vector<text_parts::Token> tokens = TokensOf(head[i], i + 1);
        if (!tokens.empty() && tokens.front().Is(kFrontWord))
        {
            if (front || tokens.size() != 2)
            {
                FailAt(i + 1, "the archive in front comes once, as a number");
            }

            const uint64_t number = text_parts::ParseInteger(tokens[1], 0xFFFF, i + 1, "the archive in front");
            front = static_cast<std::size_t>(number);
            head[i] = "";
        }
    }

    if (!front)
    {
        FailAt(*engine + 1, "a file with more than one archive names the archive in front");
    }

    project.front = *front;
    const File values = ReadText(Section(head, 1, head.size()));
    File expected;
    expected.engine = values.engine;
    expected.mixing = values.mixing;
    expected.aux_effects = values.aux_effects;
    expected.aux_volumes = values.aux_volumes;
    expected.tags = values.tags;
    expected.other_chunks = values.other_chunks;
    if (values != expected)
    {
        FailAt(head_end,
               "only the lines of the project come before the first archive: the version, the engine, the "
               "mixing mode, the archive in front, the file's tags, the effects, the volumes of the buses and "
               "the chunks that Hokuto doesn't read");
    }

    project.other_chunks = values.other_chunks;

    // Each archive takes the values of the project.
    for (std::size_t a = 0; a < archive_lines.size(); ++a)
    {
        const std::size_t first = archive_lines[a] + 1;
        const std::size_t last = a + 1 < archive_lines.size() ? archive_lines[a + 1] : lines.size();
        File archive = ReadText(Section(lines, first, last));
        if (archive.mixing != values.mixing)
        {
            FailAt(first, "an archive's mixing mode isn't the file's");
        }
        if (archive.form != ArchiveForm::kNative)
        {
            FailAt(first, "an archive keeps its native archive, not the model form");
        }
        if (archive != WithoutProjectValues(archive))
        {
            FailAt(first, "the effects, the volumes of the buses and the file's tags come before the first archive");
        }

        archive.aux_effects = values.aux_effects;
        archive.aux_volumes = values.aux_volumes;
        archive.tags = values.tags;
        project.archives.push_back(std::move(archive));
    }

    if (project.archives.size() < 2)
    {
        FailAt(*engine + 1, "a file with more than one archive has two archives or more");
    }
    if (const std::optional<std::string> problem = ProjectProblem(project))
    {
        throw xsflib::FormatError("The project " + *problem + ".");
    }

    return WithFreeFloors(std::move(project));
}

} // namespace hokuto::format
