// SPDX-License-Identifier: MIT

// Tests the Sound sections' edits of a DS sound (edit/ds_tables.h). ntdlib's builders make a small archive that the
// SDK's tools could have laid out: two sequences, a sequence archive, two banks and two players. ntdlib's reader has to
// read the new values, and the archive has to keep its size and every other byte.

#include "edit/ds_tables.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/sar/formats.h"
#include "ntdlib/sdat/files.h"
#include "ntdlib/sdat/formats.h"
#include "ntdlib/sdat/layout.h"
#include "ntdlib/testing/sdat_files.h"
#include "xsflib/testing/check.h"

#include "edit/ds_new_items.h"
#include "format/archive.h"
#include "format/file.h"
#include "format/text.h"
#include "song/ds.h"

using hokuto::format::File;
using xsflib::testing::ErrorOf;

namespace
{

namespace edit = hokuto::edit;
namespace files = ntdlib::testing::sdat_files;
namespace sar = ntdlib::sar;
namespace sdat = ntdlib::sdat;
namespace song = hokuto::song;

constexpr uint16_t kNone = 0xFFFF;

// An archive whose sounds 0 and 1 play sequence files, and whose sequence archive contains two more. Sound 1 plays on
// player `second_player`.
std::vector<uint8_t> MakeArchive(bool names, uint8_t second_player = 1)
{
    const files::Wave square{.loop = 1, .loop_length = 2, .data = {0x40, 0x40, 0xC0, 0xC0, 0x40, 0x40, 0xC0, 0xC0}};
    const std::vector<uint8_t> bytecode = {60, 0x7F, 0x18, 0xFF, 62, 0x7F, 0x18, 0xFF};
    const std::vector<sdat::ArchivedSequence> archived = {{0, 0, 80, 30, 20, 0, 0}, {4, 1, 70, 31, 21, 1, 0}};

    sdat::ArchiveModel model;
    model.sequences = {sdat::SequenceInfo{0, 0, 100, 60, 70, 0, 0, ""},
                       sdat::SequenceInfo{1, 1, 90, 50, 40, second_player, 0, ""}};
    model.sequence_archives = {sdat::SequenceArchiveInfo{2, "", {}}};
    model.banks = {sdat::BankInfo{3, {0, kNone, kNone, kNone}, ""}, sdat::BankInfo{4, {0, kNone, kNone, kNone}, ""}};
    model.wave_archives = {sdat::WaveArchiveInfo{5, ""}};
    model.players = {sdat::PlayerInfo{1, 0, 0, 0, ""}, sdat::PlayerInfo{1, 0, 0, 0, ""}};
    const files::Bytes bank = files::Bank({files::Instrument{.type = 1, .region = {.wave = 0}}});
    model.files = {files::Sequence({60, 0x7F, 0x18, 0xFF}),
                   files::Sequence({62, 0x7F, 0x18, 0xFF}),
                   sdat::SequenceArchiveFile(archived, bytecode),
                   bank,
                   bank,
                   files::WaveArchive({square})};
    if (names)
    {
        sdat::Symbols symbols;
        symbols.names[0] = {"SEQ_ONE", "SEQ_TWO"};
        symbols.names[1] = {"SSAR_ONE"};
        symbols.names[2] = {"BANK_ONE", "BANK_TWO"};
        symbols.names[3] = {"WAVES_ONE"};
        symbols.names[4] = {"PLAYER_ONE", "PLAYER_TWO"};
        symbols.sequence_names = {std::vector<std::optional<std::string>>{"SE_ONE", "SE_TWO"}};
        model.symbols = symbols;
    }

    return sdat::BuildArchive(model);
}

std::shared_ptr<const song::DsArchive> Load(const File& file)
{
    return song::LoadDsArchive(hokuto::format::ExportArchive(file), {});
}

// The archive of `edited` differs from that of `file` only within 6 bytes in a row. `edited` comes back from both its
// forms.
void CheckBytes(const File& file, const File& edited)
{
    const std::vector<uint8_t> before = hokuto::format::ExportArchive(file);
    const std::vector<uint8_t> after = hokuto::format::ExportArchive(edited);
    SEQLIB_REQUIRE(before.size() == after.size());
    std::size_t first = before.size();
    std::size_t last = 0;
    for (std::size_t i = 0; i < before.size(); i++)
    {
        if (before[i] != after[i])
        {
            first = std::min(first, i);
            last = i;
        }
    }
    XSFLIB_CHECK(first == before.size() || last - first < 6);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::Read(hokuto::format::Write(edited))) == after);
    XSFLIB_CHECK(hokuto::format::ExportArchive(hokuto::format::ReadText(hokuto::format::WriteText(edited))) == after);
}

// Sound `sound` of `edited` has `values`, and every other sound the values that it has in `before`.
void CheckSounds(const File& edited, const std::shared_ptr<const song::DsArchive>& before, uint32_t sound,
                 const edit::DsSoundValues& values)
{
    const std::shared_ptr<const song::DsArchive> after = Load(edited);
    for (const uint32_t other : song::DsSounds(*before))
    {
        const edit::DsSoundValues expected = other == sound ? values : edit::DsSoundValuesOf(*before, other);
        XSFLIB_CHECK(edit::DsSoundValuesOf(*after, other) == expected);
    }
}

// The same values of a DS sound give the same archive, in an archive with names (`names`) or without them.
void TestValues(bool names)
{
    const std::vector<uint8_t> bytes = MakeArchive(names);
    const File file = hokuto::format::ImportDsArchive(bytes);
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    const edit::DsSoundValues values{1, 12, 34, 56, 1};
    for (const uint32_t sound :
         {uint32_t{0}, uint32_t{1}, song::kFirstDsArchivedSound, song::kFirstDsArchivedSound + 1})
    {
        const File edited = edit::WithDsSoundValues(file, *archive, sound, values);
        CheckBytes(file, edited);
        CheckSounds(edited, archive, sound, values);
    }

    // The same values give the same archive.
    const File kept = edit::WithDsSoundValues(file, *archive, 1, edit::DsSoundValuesOf(*archive, 1));
    XSFLIB_CHECK(hokuto::format::ExportArchive(kept) == bytes);
}

// A change of one value of a DS sound's table entry: its name, kind, player, volume, priorities and banks read back,
// the new value reaches the archive, and a value past its limits is refused.
void TestChanges()
{
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    const uint32_t sound = song::kFirstDsArchivedSound;

    const hokuto::format::NativeSound native = edit::DsNativeSoundOf(*archive, sound);
    XSFLIB_CHECK(native.name == std::string("SE_ONE"));
    XSFLIB_CHECK(native.kind == sar::SoundType::kSequence);
    XSFLIB_CHECK(native.player == sar::ItemId(sar::kPlayerItem, 0));
    XSFLIB_CHECK(native.volume == 80);
    XSFLIB_CHECK(native.channel_priority == 30);
    XSFLIB_CHECK(native.player_priority == 20);
    XSFLIB_CHECK(native.banks == std::vector<uint32_t>{sar::ItemId(sar::kBankItem, 0)});

    edit::DsSoundValues values = edit::DsSoundValuesOf(*archive, sound);
    const auto check = [&](const edit::SoundChange& change)
    {
        const File edited = edit::ChangeDsSound(file, *archive, sound, change);
        CheckBytes(file, edited);
        CheckSounds(edited, archive, sound, values);
        XSFLIB_CHECK(edit::ValueOf(edit::DsNativeSoundOf(*Load(edited), sound), change.value, change.slot) ==
                     change.to);
    };
    values.volume = 127;
    check({edit::SoundValue::kVolume, 0, 127});
    values.volume = 80;
    values.player_priority = 127;
    check({edit::SoundValue::kPlayerPriority, 0, 127});
    values.player_priority = 20;
    values.channel_priority = 0;
    check({edit::SoundValue::kChannelPriority, 0, 0});
    values.channel_priority = 30;
    values.player = 1;
    check({edit::SoundValue::kPlayer, 0, sar::ItemId(sar::kPlayerItem, 1)});
    values.player = 0;
    values.bank = 1;
    check({edit::SoundValue::kBank, 0, sar::ItemId(sar::kBankItem, 1)});

    const auto refused = [&](const edit::SoundChange& change, const char* message)
    {
        XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::ChangeDsSound(file, *archive, sound, change); }) ==
                     std::string(message));
    };
    refused({edit::SoundValue::kVolume, 0, 128}, "The volume of a DS sound must be from 0 to 127.");
    refused({edit::SoundValue::kPlayerPriority, 0, 128}, "The player priority of a DS sound must be from 0 to 127.");
    refused({edit::SoundValue::kChannelPriority, 0, 128}, "The channel priority of a DS sound must be from 0 to 127.");
    refused({edit::SoundValue::kPlayer, 0, sar::ItemId(sar::kPlayerItem, 2)}, "The archive has no player 2.");
    refused({edit::SoundValue::kPlayer, 0, sar::kNoItem}, "A DS sound requires a player.");
    refused({edit::SoundValue::kBank, 0, sar::ItemId(sar::kBankItem, 2)}, "The archive has no bank 2.");
    refused({edit::SoundValue::kBank, 0, sar::kNoItem}, "A DS sound requires a bank.");
    refused({edit::SoundValue::kBank, 1, sar::ItemId(sar::kBankItem, 0)},
            "A DS sound has a single bank slot (slot 0).");
    refused({edit::SoundValue::kTracks, 0, 1}, "The tracks setting isn't available for DS sounds.");
}

// The DS players take their names from the archive, or numbered names where it names none.
void TestNames()
{
    const std::shared_ptr<const song::DsArchive> named = song::LoadDsArchive(MakeArchive(true), {});
    const std::vector<std::string> players = edit::DsPlayerNames(*named);
    XSFLIB_CHECK(players == (std::vector<std::string>{"PLAYER_ONE", "PLAYER_TWO"}));
    const std::shared_ptr<const song::DsArchive> unnamed = song::LoadDsArchive(MakeArchive(false), {});
    XSFLIB_CHECK(edit::DsPlayerNames(*unnamed) == (std::vector<std::string>{"PLAYER_0", "PLAYER_1"}));
}

// A game's sound can name a player that the table has no record of. Its other values still take edits, and no other
// sound can take that player.
void TestPlayerWithoutRecord()
{
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true, 5));
    const std::shared_ptr<const song::DsArchive> archive = Load(file);
    edit::DsSoundValues values = edit::DsSoundValuesOf(*archive, 1);
    XSFLIB_CHECK(values.player == 5);

    values.volume = 127;
    const File edited = edit::ChangeDsSound(file, *archive, 1, {edit::SoundValue::kVolume, 0, 127});
    CheckBytes(file, edited);
    CheckSounds(edited, archive, 1, values);

    const edit::SoundChange other = {edit::SoundValue::kPlayer, 0, sar::ItemId(sar::kPlayerItem, 5)};
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::ChangeDsSound(file, *archive, 0, other); }) ==
                 std::string("The archive has no player 5."));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { edit::WithDsSoundValues(file, *archive, 0, values); }) ==
                 std::string("The archive has no player 5."));
}

// A sound of a sequence archive keeps its tags under its number.
void TestTags()
{
    const File file = hokuto::format::ImportDsArchive(MakeArchive(true));
    const File tagged = hokuto::format::WithTag(file, song::kFirstDsArchivedSound + 1, "title", "A title");
    const auto check = [](const File& opened)
    {
        XSFLIB_CHECK(opened.sound_tags.at(song::kFirstDsArchivedSound + 1).at("title") == std::string("A title"));
    };
    check(tagged);
    check(hokuto::format::Read(hokuto::format::Write(tagged)));
    check(hokuto::format::ReadText(hokuto::format::WriteText(tagged)));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("ds_tables",
                                    []
                                    {
                                        TestValues(true);
                                        TestValues(false);
                                        TestChanges();
                                        TestNames();
                                        TestPlayerWithoutRecord();
                                        TestTags();
                                    });
}
