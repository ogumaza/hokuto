// SPDX-License-Identifier: MIT

// The sound archive that a check opens the way the program opens it: an archive of the Switch, the Wii U or the 3DS
// through ntdlib's sar, or an archive of the Wii through ntdlib's rsar. An archive of the Wii takes the game's build of
// NW4R SND from the game's main.dol, as the program's import takes it. The checks of the edits play the sounds of
// either kind through the same calls. With --model, a check keeps its file in the model form (CheckedForm), and the
// files that its edits make have to save and open again (CheckSaved).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/rvl/snd/rsar_options.h"
#include "ntdlib/sar/formats.h"
#include "ntdlib/sound_build.h"
#include "xsflib/files.h"

#include "edit/replace.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/model.h"
#include "format/text.h"
#include "song/archive.h"
#include "song/items.h"
#include "song/sequence.h"

namespace hokuto::tools
{

// What the checks read of a sound of either kind of archive.
struct CheckedSound
{
    std::string name;
    uint32_t file_id = 0;
    bool sequence = false;
};

// An archive of either kind: `sar` or `wii`. An archive of the Wii keeps the game's build of NW4R SND in `build`, or
// nothing when the check found no main.dol. The archive's version then gives the build.
struct CheckedArchive
{
    // The number of sounds in the archive's table of sounds.
    std::size_t SoundCount() const
    {
        return wii ? wii->Sounds().size() : sar->Sounds().size();
    }

    // Whether sound `sound` is a sequence sound.
    bool IsSequence(uint32_t sound) const
    {
        return wii ? wii->Sounds()[sound].type == ntdlib::rsar::SoundType::kSequence
                   : sar->Sounds()[sound].type == ntdlib::sar::SoundType::kSequence;
    }

    // The ID of the file of sound `sound` in the archive's file table.
    uint32_t FileOf(uint32_t sound) const
    {
        return wii ? wii->Sounds()[sound].file_id : sar->Sounds()[sound].file_id;
    }

    // The name of sound `sound`.
    const std::string& NameOf(uint32_t sound) const
    {
        return wii ? wii->Sounds()[sound].name : sar->Sounds()[sound].name;
    }

    // What the checks read of sound `sound`.
    CheckedSound Info(uint32_t sound) const
    {
        return {NameOf(sound), FileOf(sound), IsSequence(sound)};
    }

    // The bank in each of the sound's bank slots, by its index in the archive's bank table. A slot without a bank holds
    // nothing. A sound of the Wii plays one bank, in slot 0.
    std::vector<std::optional<uint32_t>> BanksOf(uint32_t sound) const
    {
        std::vector<std::optional<uint32_t>> banks;
        if (wii)
        {
            const uint32_t bank = wii->Sounds()[sound].bank;
            banks.push_back(bank < wii->Banks().size() ? std::optional<uint32_t>(bank) : std::nullopt);
            return banks;
        }

        for (const uint32_t slot : sar->Sounds()[sound].banks)
        {
            banks.push_back(slot == ntdlib::sar::kNoItem ? std::nullopt
                                                         : std::optional<uint32_t>(ntdlib::sar::ItemNumber(slot)));
        }

        return banks;
    }

    // The programs of bank `bank` (song::ProgramsOf).
    std::vector<uint32_t> ProgramsOf(uint32_t bank) const
    {
        return wii ? song::ProgramsOf(*wii, bank) : song::ProgramsOf(*sar, bank);
    }

    // The sound as ntdlib's performer plays it (song::ReadSequence). A sound of the Wii plays by the rules of `build`.
    song::Sequence Read(uint32_t sound, const song::StartValues& values = {}, const song::Changes& changes = {}) const
    {
        return wii ? song::ReadSequence(*wii, sound, values, changes, build)
                   : song::ReadSequence(*sar, sound, values, changes);
    }

    std::shared_ptr<const ntdlib::sar::SoundArchive> sar;
    std::shared_ptr<const ntdlib::rsar::SoundArchive> wii;
    std::optional<ntdlib::SndBuild> build;
};

// Whether `bytes` hold an archive of the Wii.
inline bool IsWiiArchive(std::span<const uint8_t> bytes)
{
    return bytes.size() >= 4 && std::memcmp(bytes.data(), "RSAR", 4) == 0;
}

// The game's build of NW4R SND for the Wii archive at `path`, as the program's import reads it: from sys/main.dol of
// the extracted disc that holds the archive (song::FindMainDol and song::ReadWiiBuild). Nothing when there's no such
// main.dol, or when it gives no build.
inline std::optional<ntdlib::SndBuild> GameBuildOf(const std::filesystem::path& path)
{
    const std::optional<std::filesystem::path> dol = song::FindMainDol(path);
    std::vector<uint8_t> code;
    return dol && xsflib::ReadFile(*dol, code) ? song::ReadWiiBuild(code) : std::nullopt;
}

// The archive at `path` with the bytes `bytes` and the group files beside it. As in the program, the Wii's archive is
// read for its model (ntdlib::rvl::snd::kRsarOptions), and takes the game's build (GameBuildOf).
inline CheckedArchive OpenChecked(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    if (IsWiiArchive(bytes))
    {
        return {nullptr,
                song::LoadWiiArchive(std::vector<uint8_t>(bytes.begin(), bytes.end()), xsflib::Utf8(path.filename()),
                                     ntdlib::rvl::snd::kRsarOptions),
                GameBuildOf(path)};
    }

    return {song::OpenArchive(path), nullptr, std::nullopt};
}

// The file that an import of `bytes`, an archive of the kind of `archive`, makes with `group_files` beside it, in the
// order of those of `archive`. Each takes its group as the program's import gives it: on the 3DS, from the name under
// which the reader found the group file of `archive` (ntdlib::sar::SoundArchive::GroupFileGroups). The Wii's archives
// have none. An archive of the Wii takes the build of `archive`, as the program's import takes the game's build.
inline format::File ImportChecked(std::span<const uint8_t> bytes, const CheckedArchive& archive,
                                  std::vector<std::vector<uint8_t>> group_files)
{
    return archive.wii ? format::ImportWiiArchive(bytes, archive.build)
                       : format::ImportArchive(bytes, archive.sar->ReadOptions().flavor, std::move(group_files), {},
                                               archive.sar->GroupFileGroups());
}

// The same, with the group files of `archive` beside `bytes`.
inline format::File ImportChecked(std::span<const uint8_t> bytes, const CheckedArchive& archive)
{
    return ImportChecked(bytes, archive, archive.sar ? archive.sar->GroupFiles() : std::vector<std::vector<uint8_t>>());
}

// The archive that `file` builds, called `name`, with the group files that the file builds. An archive of the Wii takes
// the build that the file keeps.
inline CheckedArchive BuildChecked(const format::File& file, const std::string& name)
{
    std::vector<uint8_t> bytes = format::ExportArchive(file);
    if (file.native && file.native->wii)
    {
        return {nullptr, song::LoadWiiArchive(std::move(bytes), name, ntdlib::rvl::snd::kRsarOptions),
                file.native->wii_build};
    }

    return {song::LoadArchive(bytes, name, format::ExportGroupFiles(file)), nullptr, std::nullopt};
}

// The sequence of `file` that each file of `archive` holds (format::SequencesOfFiles). `bytes` are the archive's.
inline std::vector<std::optional<uint32_t>> SequencesOfChecked(const format::File& file, std::span<const uint8_t> bytes,
                                                               const CheckedArchive& archive)
{
    return archive.wii ? format::SequencesOfFiles(file, bytes, *archive.wii)
                       : format::SequencesOfFiles(file, bytes, *archive.sar);
}

// Where each sound of `archive` plays in `file` (edit::SoundPlaces). `bytes` are the archive's.
inline std::vector<std::optional<edit::SoundPlace>> PlacesOfChecked(const format::File& file,
                                                                    std::span<const uint8_t> bytes,
                                                                    const CheckedArchive& archive)
{
    const std::vector<std::optional<uint32_t>> sequences = SequencesOfChecked(file, bytes, archive);
    const std::vector<std::optional<uint32_t>> starts = format::StartFieldsOfSounds(file, bytes);
    return archive.wii ? edit::SoundPlaces(*archive.wii, sequences, starts)
                       : edit::SoundPlaces(*archive.sar, sequences, starts);
}

// Where two ranges `a` and `b` first differ, for a check's message: their sizes when those differ, or else the first
// index at which their elements differ.
template <class Range>
std::string FirstDifference(const Range& a, const Range& b)
{
    if (a.size() != b.size())
    {
        return "sizes " + std::to_string(a.size()) + " and " + std::to_string(b.size());
    }

    std::size_t index = 0;
    while (index < a.size() && a[index] == b[index])
    {
        ++index;
    }

    return "first at " + std::to_string(index);
}

// The parts in which files `a` and `b` differ, for a check's message: each part of the file and of its native archive
// that differs, with where a list in it first differs.
inline std::string PartsThatDiffer(const format::File& a, const format::File& b)
{
    std::string parts;
    const auto add = [&parts](bool same, const std::string& part)
    {
        if (!same)
        {
            parts += (parts.empty() ? " " : "; ") + part;
        }
    };

    add(a.engine == b.engine && a.sound_engines == b.sound_engines, "the engines");
    add(a.mixing == b.mixing && a.sound_mixing == b.sound_mixing, "the mixing modes");
    add(a.aux_effects == b.aux_effects && a.aux_volumes == b.aux_volumes, "the effects");
    add(a.scenes == b.scenes, "the scenes");
    add(a.track_colours == b.track_colours, "the colours");
    add(a.tags == b.tags && a.sound_tags == b.sound_tags, "the tags");
    add(a.lengths == b.lengths, "the lengths");
    add(a.form == b.form, "the form");
    add(a.other_chunks == b.other_chunks, "the other chunks");
    add(a.view == b.view, "the view");
    add(a.banks == b.banks, "the banks");
    add(a.waves == b.waves, "the waves");
    add(a.timelines == b.timelines, "the timelines");
    add(a.sequences == b.sequences, "the sequences, " + FirstDifference(a.sequences, b.sequences));
    if (!a.native || !b.native)
    {
        add(!a.native == !b.native, "the native archive");
        return parts;
    }

    const format::NativeArchive& native_a = *a.native;
    const format::NativeArchive& native_b = *b.native;
    add(native_a.flavor == native_b.flavor && native_a.wii == native_b.wii, "the archive's kind");
    add(native_a.pieces == native_b.pieces, "the pieces, " + FirstDifference(*native_a.pieces, *native_b.pieces));
    add(native_a.fields == native_b.fields, "the fields, " + FirstDifference(native_a.fields, native_b.fields));
    add(native_a.group_files == native_b.group_files,
        "the group files, " + FirstDifference(native_a.group_files, native_b.group_files));
    if (native_a.tables && native_b.tables)
    {
        add(native_a.tables->sounds == native_b.tables->sounds,
            "the sounds' values, " + FirstDifference(native_a.tables->sounds, native_b.tables->sounds));
        add(native_a.tables->banks == native_b.tables->banks &&
                native_a.tables->wave_archives == native_b.tables->wave_archives &&
                native_a.tables->players == native_b.tables->players &&
                native_a.tables->groups == native_b.tables->groups,
            "the other tables");
    }
    else
    {
        add(!native_a.tables == !native_b.tables, "the tables");
    }
    add(native_a.game_sizes == native_b.game_sizes, "the game's sizes");
    add(native_a.streams == native_b.streams, "the streams");
    add(native_a.wii_build == native_b.wii_build, "the build");

    return parts;
}

// Whether the check runs with --model (TakeModelOption).
inline bool model_form = false;

// The saves that CheckSaved checked, the saves of samples that it saw, and those that it passed over.
inline std::size_t checked_saves = 0;
inline std::size_t sampled_saves = 0;
inline std::size_t passed_saves = 0;

// The saves of a sample that CheckSaved checks: the first kAllSampledSaves of the run, then every kSampleStep-th.
inline constexpr std::size_t kAllSampledSaves = 64;
inline constexpr std::size_t kSampleStep = 64;

// Takes --model out of `arguments`, wherever it stands, and sets model_form when it was there.
inline void TakeModelOption(std::vector<std::string>& arguments)
{
    model_form = std::erase(arguments, std::string("--model")) > 0;
}

// `file` in the model form when the check runs with --model (format::InModelForm), or else `file`. An import of `bytes`
// made `file`. The edits then work on the archive that the model builds. Throws std::runtime_error when that archive
// differs from `bytes`, or its group files from `file`'s, and the exceptions of InModelForm.
inline format::File CheckedForm(format::File file, std::span<const uint8_t> bytes)
{
    if (!model_form)
    {
        return file;
    }

    const std::vector<std::vector<uint8_t>> group_files = format::ExportGroupFiles(file);
    format::File model = format::InModelForm(std::move(file));
    if (!std::ranges::equal(format::ExportArchive(model), bytes) || format::ExportGroupFiles(model) != group_files)
    {
        throw std::runtime_error("the archive that the model form builds differs from the game's");
    }

    return model;
}

// With --model, saves `file`, a file as the program saves it, in both forms, and opens each form again. Each form has
// to open as `file`, and a save of the opened file has to give the same bytes again. Returns whether it checked the
// save. In a `sample` of many saves, it checks the first kAllSampledSaves of the run, then every kSampleStep-th. Throws
// std::runtime_error with what differs, and the exceptions of the saves and the opens.
inline bool CheckSaved(const format::File& file, bool sample = false)
{
    if (!model_form)
    {
        return false;
    }
    if (sample && ++sampled_saves > kAllSampledSaves && sampled_saves % kSampleStep != 0)
    {
        ++passed_saves;
        return false;
    }

    // The opened file cuts the archive that the model builds. The opened file's pieces hold the tables' values. An edit
    // leaves the old values in the pieces. An edit that grows the tables also leaves the fields at their old places.
    // The archives that the pieces build have to be the same.
    const std::vector<uint8_t> binary = format::Write(file);
    const format::File opened = format::Read(binary);
    format::File compared = opened;
    if (opened.native && file.native)
    {
        format::NativeArchive native = *opened.native;
        native.pieces = file.native->pieces;
        native.fields = file.native->fields;
        for (std::size_t i = 0; i < native.group_files.size() && i < file.native->group_files.size(); ++i)
        {
            native.group_files[i].pieces = file.native->group_files[i].pieces;
            native.group_files[i].fields = file.native->group_files[i].fields;
        }
        compared.native = std::move(native);
    }
    if (!(compared == file))
    {
        throw std::runtime_error("the saved file opens otherwise from the binary form:" +
                                 PartsThatDiffer(compared, file));
    }
    if (format::ExportArchive(opened) != format::ExportArchive(file) ||
        format::ExportGroupFiles(opened) != format::ExportGroupFiles(file))
    {
        throw std::runtime_error("the saved file opens from the binary form with another archive");
    }
    const format::File from_text = format::ReadText(format::WriteText(file));
    if (!(from_text == opened))
    {
        throw std::runtime_error("the saved file opens otherwise from the text form:" +
                                 PartsThatDiffer(from_text, opened));
    }
    if (format::Write(opened) != binary)
    {
        throw std::runtime_error("a save of the opened file gives other bytes");
    }
    ++checked_saves;

    return true;
}

// With --model, prints the line of `name` that counts CheckSaved's saves.
inline void PrintSaves(const std::string& name)
{
    if (!model_form)
    {
        return;
    }

    std::printf("%s: %zu files saved in the model form opened again in both forms.", name.c_str(), checked_saves);
    if (passed_saves > 0)
    {
        std::printf(" %zu saves between the samples went unchecked.", passed_saves);
    }
    std::printf("\n");
}

// Prints PrintSaves's line of `name` when it goes out of scope: after the lines that a check prints on its way out,
// whichever way it returns.
class SavesLine
{
public:
    // The line of the check `name`.
    explicit SavesLine(std::string name) : name_(std::move(name))
    {
    }

    SavesLine(const SavesLine&) = delete;
    SavesLine& operator=(const SavesLine&) = delete;

    // Prints the line.
    ~SavesLine()
    {
        PrintSaves(name_);
    }

private:
    std::string name_;
};

// Whether `file` saves and opens again as it was, in both forms. In the native form, each form has to open as `file`.
// With --model, CheckSaved checks the save instead. When `sample` is true, the save counts as one of a sample.
// CheckSaved's exceptions pass through. A save that the sample passes over counts as one that opens again.
inline bool SavesAndOpens(const format::File& file, bool sample = false)
{
    if (model_form)
    {
        CheckSaved(file, sample);
        return true;
    }

    return format::Read(format::Write(file)) == file && format::ReadText(format::WriteText(file)) == file;
}

} // namespace hokuto::tools
