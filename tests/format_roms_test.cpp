// SPDX-License-Identifier: MIT

// Tests GBA ROMs, DS archives and images and DSE images into Hokuto's files and back (src/format/archive.cpp).

#include "cart_bytes.h"
#include "format_testing.h"

namespace
{

// A cartridge of MP2K songs. Song 0's track leaves out the key and the velocity of its second note, and its third note
// repeats the running status. Song 1 loops on a note. The bytes from `padding` on are 0xFF. A cartridge of `size` bytes
// past 0x40000 has zeros after them.
std::vector<uint8_t> GbaRom(uint32_t padding = 0x38000, uint32_t size = 0x40000)
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;

    mp2k_cart::Cart cart;
    cart.Resize(size);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    mp2k_cart::Track song;
    song.Cmd(kCmdTempo, 75).Cmd(kCmdVoice, 0).Note(24, 60, 100).Wait(24).Note(24).Wait(24).Raw({62}).Wait(24).Fine();
    cart.Song({song}, voices);

    mp2k_cart::Track looping;
    looping.Cmd(kCmdVoice, 0);
    const std::size_t loop = looping.Here();
    looping.Note(24, 64, 90).Wait(24).Goto(loop);
    cart.Song({looping}, voices);
    for (uint32_t a = padding; a < 0x40000; a++)
    {
        cart.Put8(mp2k_cart::kRomBase + a, 0xFF);
    }

    const ntdlib::agb::Rom rom = cart.ToRom();
    return hokuto::testing::RomBytes(rom);
}

// The ROM from GbaRom and the file that imports it.
struct GbaRomFixture
{
    std::vector<uint8_t> rom;
    File file;
};

// The fixture of the tests of a GBA ROM.
GbaRomFixture MakeGbaRomFixture()
{
    const std::vector<uint8_t> rom = GbaRom();
    const File file = format::ImportGbaRom(rom);

    return {rom, file};
}

// The file gets the GBA engine, and the size of the ROM as the size of the game. Each song is a sequence, and the
// export returns the ROM unchanged.
void TestGbaRomImport()
{
    const GbaRomFixture fixture = MakeGbaRomFixture();
    const std::vector<uint8_t>& rom = fixture.rom;
    const File& file = fixture.file;

    SEQLIB_REQUIRE(file.engine == Engine::kGba && file.native && file.native->engine == format::Engine::kGba &&
                   file.native->engine != format::Engine::kWii);
    XSFLIB_CHECK(file.sequences.size() == 2 && !file.native->tables);
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{rom.size(), {}}));
    XSFLIB_CHECK(format::ExportArchive(file) == rom && format::ExportGroupFiles(file).empty());
    XSFLIB_CHECK(format::IsGbaRom(std::vector<uint8_t>(0xC0, 0)) == false);

    // A sound archive and a text file of Hokuto can hold the GBA header's fixed value at 0xB2.
    std::vector<uint8_t> header(0xC0, 0);
    header[0xB2] = 0x96;
    XSFLIB_CHECK(format::IsGbaRom(header));
    for (const std::string_view signature : {"RSAR", "CSAR", "FSAR", "SDAT", "hok "})
    {
        std::vector<uint8_t> other = header;
        std::copy(signature.begin(), signature.end(), other.begin());
        XSFLIB_CHECK(!format::IsGbaRom(other));
    }

    // The second note leaves out its arguments, and the third repeats the running status.
    const std::vector<Command>& items = file.sequences[0]->items;
    const auto chosen = std::count_if(items.begin(), items.end(), format::HasMp2kChoices);
    XSFLIB_CHECK(chosen >= 2 && std::none_of(items.begin(), items.end(), format::HasNwsndChoices));
    XSFLIB_CHECK(std::any_of(items.begin(), items.end(), [](const Command& item) { return item.encoding.repeated; }));
}

// A GBA ROM, in both forms.
void TestGbaRomForms()
{
    const GbaRomFixture fixture = MakeGbaRomFixture();
    const File& file = fixture.file;

    // Both forms give the file back. The engine and the archive get number 5 and the name gba, and the piece of a song
    // records the room of its place.
    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nengine gba\n") != std::string::npos && text.find("\nnative gba\n") != std::string::npos);
    XSFLIB_CHECK(text.find("sequence_file 0 room ") != std::string::npos &&
                 text.find(" version ") == std::string::npos);
    XSFLIB_CHECK(text.find(" [repeated]") != std::string::npos && text.find(" [arguments 0]") != std::string::npos);

    // A value that MP2K's bytes can't store reports its line, by MP2K's rules: a program past 127, and a pan between
    // two of MP2K's values.
    std::string wrong = text;
    const std::size_t program = wrong.find("        program 0\n");
    SEQLIB_REQUIRE(program != std::string::npos);
    const auto line = std::count(wrong.begin(), wrong.begin() + static_cast<std::ptrdiff_t>(program), '\n') + 1;
    wrong.replace(program, 18, "        program 300\n");
    XSFLIB_CHECK(TextError(wrong).starts_with("Line " + std::to_string(line) + ": program 300"));
    wrong.replace(program, 20, "        pan 0.5\n");
    XSFLIB_CHECK(TextError(wrong).starts_with("Line " + std::to_string(line) + ": pan has to take one of MP2K's"));
}

// A song that outgrows its place moves into the ROM's free space.
void TestGbaRomLongerSong()
{
    const GbaRomFixture fixture = MakeGbaRomFixture();
    const std::vector<uint8_t>& rom = fixture.rom;
    const File& file = fixture.file;

    // A longer song moves to the free space, 256 bytes into the run of 0xFF bytes, and comes back from the export.
    File more = file;
    more.sequences[0].Edit([](Sequence& sequence)
                           { sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[0]); });
    const std::vector<uint8_t> grown = format::ExportArchive(more);
    XSFLIB_CHECK(grown.size() == rom.size() && grown != rom);
    // The cut reads the songs in the order of their places: the moved song now comes after the other.
    const File moved = format::ImportGbaRom(grown);
    SEQLIB_REQUIRE(moved.sequences.size() == 2);
    XSFLIB_CHECK(format::ExportArchive(moved) == grown);
    XSFLIB_CHECK(*moved.sequences[0] == *file.sequences[1]);
    // The song place contained zeros up to its header, and the free space contains others up to the header copy.
    const auto without_zeros = [](std::vector<Command> kept)
    {
        while (!kept.empty() && kept.back().kind == Kind::kData &&
               std::all_of(kept.back().bytes.begin(), kept.back().bytes.end(), [](uint8_t b) { return b == 0; }))
        {
            kept.pop_back();
        }

        return kept;
    };
    XSFLIB_CHECK(without_zeros(moved.sequences[1]->items) == without_zeros(more.sequences[0]->items));
}

// MP2K's choices of the encoder go with each song's piece, and the other consoles' archives refuse them. An import
// refuses bytes without MusicPlayer2000, and the model form refuses a GBA ROM.
void TestGbaRomRefusals()
{
    const GbaRomFixture fixture = MakeGbaRomFixture();
    const File& file = fixture.file;

    // MP2K's choices belong to a GBA ROM, and the other choices to the other consoles' archives.
    File foreign = format::ImportSwitchArchive(Archive());
    foreign.sequences[0].Edit([](Sequence& sequence) { sequence.items[0].encoding.repeated = true; });
    XSFLIB_CHECK(format::ForeignChoices(foreign) == 0);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(foreign); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(foreign); }));
    File wide = file;
    wide.sequences[1].Edit([](Sequence& sequence) { sequence.items[0].encoding.value_bytes = 2; });
    XSFLIB_CHECK(format::ForeignChoices(wide) == 1 && !format::ForeignChoices(file));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(wide); }));
    std::string switch_text = format::WriteText(format::ImportSwitchArchive(Archive()));
    const std::size_t volume = switch_text.find("volume ");
    SEQLIB_REQUIRE(volume != std::string::npos);
    switch_text.insert(switch_text.find('\n', volume), " [repeated]");
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::ReadText(switch_text); }));

    // Bytes without MusicPlayer2000, and the model form.
    XSFLIB_CHECK(Throws<xsflib::FormatError>([] { format::ImportGbaRom(std::vector<uint8_t>(0x1000, 0)); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::InModelForm(file); }));
}

// A sound archive of the DS with two sequence files and another file between them. The first sequence pans and plays a
// note. The second loops on a note.
std::vector<uint8_t> DsArchive()
{
    namespace sdat_files = ntdlib::testing::sdat_files;
    sdat_files::Archive spec;
    spec.items[0] = {sdat_files::SequenceItem(0, 0, "SEQ_A"), sdat_files::SequenceItem(2, 0, "SEQ_B")};
    spec.files = {sdat_files::Sequence({0xFE, 0x01, 0x00, 0xC0, 0x40, 0x3C, 0x7F, 0x30, 0xFF}),
                  std::vector<uint8_t>(40, 0xAB),
                  sdat_files::Sequence({0xFE, 0x01, 0x00, 0x3C, 0x7F, 0x30, 0x94, 0x03, 0x00, 0x00})};

    return sdat_files::SoundArchive(spec);
}

// A sound archive of the DS, in both forms. The file gets the DS engine, and the size of the archive as the size of the
// game. Each sequence file is a sequence, and the export returns the archive unchanged. A longer sequence moves the
// files after it. The text form checks the commands by the DS's rules. An import refuses bytes that aren't an archive,
// and the model form refuses a DS archive. A new file of the DS keeps no game's size.
void TestDsArchive()
{
    const std::vector<uint8_t> sdat = DsArchive();
    const File file = format::ImportDsArchive(sdat);

    SEQLIB_REQUIRE(file.engine == Engine::kDs && file.native && file.native->engine == format::Engine::kDs &&
                   file.native->engine != format::Engine::kGba);
    XSFLIB_CHECK(file.sequences.size() == 2 && !file.native->tables && !file.native->cartridge);
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{sdat.size(), {}}));
    XSFLIB_CHECK(format::ExportArchive(file) == sdat && format::ExportGroupFiles(file).empty());
    XSFLIB_CHECK(format::IsDsArchive(sdat) && !format::IsDsImage(sdat));
    XSFLIB_CHECK(format::CodeOf(Engine::kDs) == 6 && format::EngineNamed("ds") == Engine::kDs);

    // Both forms give the file back. The engine and the archive take number 6 and the name ds.
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(format::Write(file)) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nengine ds\n") != std::string::npos && text.find("\nnative ds\n") != std::string::npos);
    XSFLIB_CHECK(text.find("sequence_file 1 version 0x100") != std::string::npos);

    // A pan between two of the DS's values reports its line.
    std::string wrong = text;
    const std::size_t pan = wrong.find("        pan ");
    SEQLIB_REQUIRE(pan != std::string::npos);
    const auto line = std::count(wrong.begin(), wrong.begin() + static_cast<std::ptrdiff_t>(pan), '\n') + 1;
    wrong.replace(pan, wrong.find('\n', pan) - pan, "        pan 0.001");
    XSFLIB_CHECK(TextError(wrong).starts_with("Line " + std::to_string(line) + ": pan has to take one of the DS's"));

    // 20 more pans make the first sequence file 40 bytes longer. The files after it move by 32 bytes, to keep their
    // places on multiples of 32 bytes.
    File more = file;
    more.sequences[0].Edit([](Sequence& sequence)
                           { sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[1]); });
    const std::vector<uint8_t> grown = format::ExportArchive(more);
    XSFLIB_CHECK(grown.size() == sdat.size() + 32);
    const File moved = format::ImportDsArchive(grown);
    SEQLIB_REQUIRE(moved.sequences.size() == 2);
    XSFLIB_CHECK(*moved.sequences[0] == *more.sequences[0] && *moved.sequences[1] == *file.sequences[1]);
    XSFLIB_CHECK(format::ExportArchive(moved) == grown);

    // Bytes that aren't an archive, and the model form.
    XSFLIB_CHECK(Throws<xsflib::FormatError>([] { format::ImportDsArchive(std::vector<uint8_t>(64, 0)); }));

    // A header that passes its checks, then nothing: the message says so, not where a read failed.
    std::vector<uint8_t> cut(64, 0);
    const std::array<uint8_t, 8> header = {'S', 'D', 'A', 'T', 0xFF, 0xFE, 0x00, 0x01};
    std::copy(header.begin(), header.end(), cut.begin());
    XSFLIB_CHECK(xsflib::testing::ErrorOf<xsflib::FormatError>([&] { format::ImportDsArchive(cut); }) ==
                 std::string("the file is cut short, or it isn't a DS sound archive"));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::InModelForm(file); }));
    XSFLIB_CHECK(format::NewFile(Engine::kDs).engine == Engine::kDs);

    // A new file has no game, and its export measures no growth.
    XSFLIB_CHECK(!format::NewFile(Engine::kDs).native->game_sizes);
}

// A DS cartridge image with a sound archive in its file system.
struct DsImageFixture
{
    std::vector<uint8_t> sdat;
    std::vector<uint8_t> after;
    ntdlib::testing::nds_files::Image spec;
    std::vector<uint8_t> image;
    std::vector<uint32_t> ids;
    File file;
    File more;
    format::DsImageExport grown;
};

// The fixture of the tests of a DS image with a sound archive. The fixture also exports the file with a longer
// sequence.
DsImageFixture MakeDsImageFixture()
{
    namespace nds_files = ntdlib::testing::nds_files;
    const std::vector<uint8_t> sdat = DsArchive();
    const std::vector<uint8_t> after(0x300, 0xC3);
    nds_files::Image spec;
    spec.files = {{"sound/sound_data.sdat", sdat}, {"extra/after.bin", after}};
    std::vector<uint8_t> image = nds_files::BuildImage(spec).first;
    const std::vector<uint32_t> ids = nds_files::BuildImage(spec).second;
    image.resize(image.size() + 0x400, 0xFF);

    const File file = format::ImportDsImage(image);

    File more = file;
    more.sequences[0].Edit([](Sequence& sequence)
                           { sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[1]); });
    const format::DsImageExport grown = format::ExportDsImage(more);

    return {sdat, after, spec, image, ids, file, more, grown};
}

// The import finds the archive, and the file keeps the image. Both forms keep the image in the CART chunk, without the
// 0xFF bytes at its end.
void TestDsImageImport()
{
    const DsImageFixture fixture = MakeDsImageFixture();
    const std::vector<uint8_t>& sdat = fixture.sdat;
    const std::vector<uint8_t>& image = fixture.image;
    const std::vector<uint32_t>& ids = fixture.ids;
    const File& file = fixture.file;

    const std::vector<format::DsArchivePlace> places = format::DsArchivesIn(image);
    SEQLIB_REQUIRE(places.size() == 1);
    XSFLIB_CHECK(
        (places[0] == format::DsArchivePlace{ids[0], "sound/sound_data.sdat", 0, static_cast<uint32_t>(sdat.size())}));

    SEQLIB_REQUIRE(file.native && file.native->cartridge);
    const format::Cartridge& cartridge = *file.native->cartridge;
    XSFLIB_CHECK(cartridge.file == ids[0] && cartridge.offset == 0 && cartridge.size == image.size());
    XSFLIB_CHECK(cartridge.image->size() <= image.size() - 0x400 && cartridge.image->back() != 0xFF);
    XSFLIB_CHECK(format::CartridgeImage(cartridge) == image);
    const format::DsImageExport same = format::ExportDsImage(file);
    XSFLIB_CHECK(same.image == image && same.stale.empty());

    // Both forms give the file back.
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(format::Write(file)) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nchunk CART ") != std::string::npos);
}

// The export returns the image unchanged, and a longer sequence moves the files after the archive.
void TestDsImageLongerSequence()
{
    const DsImageFixture fixture = MakeDsImageFixture();
    const std::vector<uint8_t>& after = fixture.after;
    const std::vector<uint8_t>& image = fixture.image;
    const std::vector<uint32_t>& ids = fixture.ids;
    const File& more = fixture.more;
    const format::DsImageExport& grown = fixture.grown;

    // A longer sequence: the archive's file grows, and the file after it keeps its bytes.
    XSFLIB_CHECK(grown.image != image && grown.stale.empty());
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(grown.image);
    XSFLIB_CHECK(std::ranges::equal(rom.FileData(ids[0]), format::ExportArchive(more)));
    XSFLIB_CHECK(std::ranges::equal(rom.FileData(ids[1]), after));
    const File again = format::ImportDsImage(grown.image);
    SEQLIB_REQUIRE(again.sequences.size() == 2);
    XSFLIB_CHECK(*again.sequences[0] == *more.sequences[0]);
    XSFLIB_CHECK(format::ExportDsImage(again).image == grown.image);
}

// The export keeps an archive in its place when the file of the archive contains other data too.
void TestDsImageArchiveInContainer()
{
    namespace nds_files = ntdlib::testing::nds_files;

    const DsImageFixture fixture = MakeDsImageFixture();
    const std::vector<uint8_t>& sdat = fixture.sdat;
    const File& more = fixture.more;

    // An archive after other data in its file, such as an archive in a container, and an archive before such data. The
    // export keeps each archive in its place, and refuses a longer one: something may point into the other data.
    std::vector<uint8_t> inner(0x40, 0x5A);
    inner.insert(inner.end(), sdat.begin(), sdat.end());
    std::vector<uint8_t> before = sdat;
    before.insert(before.end(), 0x40, 0x5A);
    for (const auto& [shared, offset] : {std::pair{&inner, 0x40u}, std::pair{&before, 0u}})
    {
        nds_files::Image container;
        container.files = {{"sound/pack.bin", *shared}};
        const std::vector<uint8_t> packed = nds_files::BuildImage(container).first;
        const std::vector<format::DsArchivePlace> found = format::DsArchivesIn(packed);
        SEQLIB_REQUIRE(found.size() == 1);
        XSFLIB_CHECK(found[0].offset == offset);
        File in_pack = format::ImportDsImage(packed, found[0]);
        XSFLIB_CHECK(format::ExportDsImage(in_pack).image == packed);
        in_pack.sequences[0] = more.sequences[0];
        XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::ExportDsImage(in_pack); }));
    }
}

// The image runs on the DS alone: its unit code is 0. The DSi's digest key changes nothing in its export. A key file
// contains the 64 bytes of the key.
void TestDsImageDsi()
{
    namespace nds_files = ntdlib::testing::nds_files;

    const DsImageFixture fixture = MakeDsImageFixture();
    const nds_files::Image& spec = fixture.spec;
    const std::vector<uint8_t>& image = fixture.image;
    const File& more = fixture.more;
    const format::DsImageExport& grown = fixture.grown;

    // The DSi's digest key, and the unit code of an image that runs on the DSi too.
    std::vector<uint8_t> key_file(64);
    for (std::size_t i = 0; i < key_file.size(); i++)
    {
        key_file[i] = static_cast<uint8_t>(0xA5 ^ i);
    }
    const ntdlib::ntr::DsiKeys keys = format::DsiKeysOf(key_file);
    XSFLIB_CHECK(std::ranges::equal(keys.digest, key_file));
    XSFLIB_CHECK(Throws<std::invalid_argument>([] { format::DsiKeysOf(std::vector<uint8_t>(63)); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([] { format::DsiKeysOf(std::vector<uint8_t>(65)); }));
    XSFLIB_CHECK(format::ExportDsImage(more, keys).image == grown.image);
    XSFLIB_CHECK(!format::IsDsiImage(image));
    nds_files::Image dsi_spec = spec;
    dsi_spec.unit_code = 2;
    XSFLIB_CHECK(format::IsDsiImage(nds_files::BuildImage(dsi_spec).first));
    dsi_spec.unit_code = 3;
    XSFLIB_CHECK(format::IsDsiImage(nds_files::BuildImage(dsi_spec).first));
}

// Only an archive of the DS accepts a cartridge. An import refuses an image without a sound archive.
void TestDsImageRefusals()
{
    namespace nds_files = ntdlib::testing::nds_files;

    const DsImageFixture fixture = MakeDsImageFixture();
    const std::vector<uint8_t>& after = fixture.after;
    const File& file = fixture.file;

    SEQLIB_REQUIRE(file.native && file.native->cartridge);
    const format::Cartridge& cartridge = *file.native->cartridge;

    // A cartridge belongs to an archive of the DS, and an image without a sound archive has none.
    File wrong = format::ImportSwitchArchive(Archive());
    wrong.native.Edit([&](NativeArchive& native) { native.cartridge = cartridge; });
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(wrong); }));
    nds_files::Image empty;
    empty.files = {{"extra/after.bin", after}};
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::ImportDsImage(nds_files::BuildImage(empty).first); }));
}

// A DS cartridge image with DSE's files in its file system: a song, a wave bank and a bank of two sound effects.
struct DseImageFixture
{
    std::vector<uint8_t> song;
    std::vector<uint8_t> bank;
    std::vector<uint8_t> effects;
    std::vector<uint8_t> after;
    std::vector<uint8_t> image;
    std::vector<uint32_t> ids;
    File file;
};

// The fixture of the tests of a DS image with DSE's files.
DseImageFixture MakeDseImageFixture()
{
    namespace nds_files = ntdlib::testing::nds_files;
    namespace dse_files = ntdlib::testing::dse_song_files;
    const std::vector<uint8_t> song = dse_files::SongFile();
    const std::vector<uint8_t> bank = dse_files::BankFile();
    const std::vector<uint8_t> effects =
        dse_files::EffectsFile({dse_files::Effect{.tracks = {{0, 0, 0, 0, 0x40, 0x65, 0x30, 0x92, 0x30, 0x98}}},
                                dse_files::Effect{.tracks = {{0, 0, 0, 0, 0x40, 0x67, 0x18, 0x92, 0x18, 0x98}}}},
                               0x6430);
    const std::vector<uint8_t> after(0x300, 0xC3);
    nds_files::Image spec;
    spec.files = {{"SOUND/BGM/bgm.smd", song},
                  {"SOUND/BGM/bgm.swd", bank},
                  {"SOUND/SE/se.sed", effects},
                  {"extra/after.bin", after}};
    std::vector<uint8_t> image = nds_files::BuildImage(spec).first;
    const std::vector<uint32_t> ids = nds_files::BuildImage(spec).second;
    image.resize(image.size() + 0x400, 0xFF);
    const File file = format::ImportDseImage(image);

    return {song, bank, effects, after, image, ids, file};
}

// The import reads the song and the effects as sequences. The file keeps the image. The export returns it unchanged, in
// both forms.
void TestDseImageImport()
{
    const DseImageFixture fixture = MakeDseImageFixture();
    const std::vector<uint8_t>& song = fixture.song;
    const std::vector<uint8_t>& bank = fixture.bank;
    const std::vector<uint8_t>& effects = fixture.effects;
    const std::vector<uint8_t>& image = fixture.image;
    const File& file = fixture.file;

    XSFLIB_CHECK(format::HoldsDseFiles(image));
    SEQLIB_REQUIRE(file.engine == Engine::kDse && file.native && file.native->engine == format::Engine::kDse &&
                   file.native->engine != format::Engine::kDs);
    SEQLIB_REQUIRE(file.sequences.size() == 3 && file.native->cartridge);
    XSFLIB_CHECK(file.sequences[0]->labels.at(0).name == "SOUND_BGM_bgm_smd");
    XSFLIB_CHECK(file.sequences[1]->labels.at(0).name == "SOUND_SE_se_sed_0");
    XSFLIB_CHECK(file.sequences[2]->labels.at(0).name == "SOUND_SE_se_sed_1");
    XSFLIB_CHECK(format::CodeOf(Engine::kDse) == 7 && format::EngineNamed("dse") == Engine::kDse);
    const format::Cartridge& cartridge = *file.native->cartridge;
    XSFLIB_CHECK(cartridge.file == 0 && cartridge.offset == 0 && cartridge.size == image.size());
    XSFLIB_CHECK(format::CartridgeImage(cartridge) == image);
    const format::DsImageExport same = format::ExportDsImage(file);
    XSFLIB_CHECK(same.image == image && same.stale.empty());

    // The set contains the game's files.
    const std::vector<uint8_t> set = format::ExportArchive(file);
    XSFLIB_CHECK((file.native->game_sizes == GameSizes{set.size(), {}}));
    const std::vector<ntdlib::dse::NamedFile> files = ntdlib::dse::ReadSet(set);
    SEQLIB_REQUIRE(files.size() == 3);
    XSFLIB_CHECK(std::ranges::equal(files[0].bytes, song) && std::ranges::equal(files[1].bytes, bank) &&
                 std::ranges::equal(files[2].bytes, effects));
}

// The text form checks the commands by DSE's rules, and only a set of DSE's files accepts a field of 2 bytes.
void TestDseImageForms()
{
    const DseImageFixture fixture = MakeDseImageFixture();
    const File& file = fixture.file;

    // Both forms give the file back, the encoder's choices and the fields of 2 bytes included.
    const std::string text = format::WriteText(file);
    XSFLIB_CHECK(format::Read(format::Write(file)) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nengine dse\n") != std::string::npos && text.find("\nnative dse\n") != std::string::npos);
    XSFLIB_CHECK(text.find(" bytes 2\n") != std::string::npos && text.find("\nchunk CART ") != std::string::npos);

    File choices = file;
    choices.sequences[0].Edit(
        [](Sequence& sequence)
        {
            const auto note = std::ranges::find(sequence.items, Kind::kNote, &ntdlib::commands::Command::kind);
            SEQLIB_REQUIRE(note != sequence.items.end());
            note->encoding.value_bytes = 3;
        });
    XSFLIB_CHECK(format::Read(format::Write(choices)) == choices);
    XSFLIB_CHECK(format::ReadText(format::WriteText(choices)) == choices);
    File foreign = file;
    foreign.sequences[0].Edit([](Sequence& sequence) { sequence.items[0].encoding.unused_random = true; });
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(foreign); }));

    // A pan between two of DSE's values reports its line, and a field of 2 bytes outside a set reports its line.
    std::string wrong = text;
    const std::size_t pan = wrong.find("        pan ");
    SEQLIB_REQUIRE(pan != std::string::npos);
    const auto pan_line = std::count(wrong.begin(), wrong.begin() + static_cast<std::ptrdiff_t>(pan), '\n') + 1;
    wrong.replace(pan, wrong.find('\n', pan) - pan, "        pan 0.001");
    XSFLIB_CHECK(TextError(wrong).starts_with("Line " + std::to_string(pan_line) + ": pan has to take one of DSE's"));

    // A note that DSE's bytes can't store reports the reason of the encoder in both forms, with one full stop.
    File loud = file;
    loud.sequences[0].Edit(
        [](Sequence& sequence)
        {
            const auto note = std::ranges::find(sequence.items, Kind::kNote, &ntdlib::commands::Command::kind);
            SEQLIB_REQUIRE(note != sequence.items.end());
            note->velocity = 200 * kOne;
        });
    const std::string reason = "A DSE track can't hold a note of velocity 200.";
    XSFLIB_CHECK(TextError(format::WriteText(loud)).ends_with(": " + reason));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::ExportArchive(loud); }) == reason);
    std::string ds_text = format::WriteText(format::ImportDsArchive(DsArchive()));
    const std::size_t field = ds_text.find("\n    field ");
    SEQLIB_REQUIRE(field != std::string::npos);
    const std::size_t field_end = ds_text.find('\n', field + 1);
    const auto field_line = std::count(ds_text.begin(), ds_text.begin() + static_cast<std::ptrdiff_t>(field_end), '\n');
    ds_text.insert(field_end, " bytes 2");
    XSFLIB_CHECK(TextError(ds_text).starts_with("Line " + std::to_string(field_line + 1) +
                                                ": only DSE files and ROMs with Konami's driver"));
}

// A longer song and a longer effect move what follows, and every other file keeps its bytes.
void TestDseImageLongerSong()
{
    const DseImageFixture fixture = MakeDseImageFixture();
    const std::vector<uint8_t>& song = fixture.song;
    const std::vector<uint8_t>& bank = fixture.bank;
    const std::vector<uint8_t>& effects = fixture.effects;
    const std::vector<uint8_t>& after = fixture.after;
    const std::vector<uint8_t>& image = fixture.image;
    const std::vector<uint32_t>& ids = fixture.ids;
    const File& file = fixture.file;

    // A longer song and a longer effect: the files of the song and the bank grow, and the other files keep their bytes.
    File more = file;
    for (const std::size_t sequence : {std::size_t{0}, std::size_t{2}})
    {
        more.sequences[sequence].Edit(
            [](Sequence& edited)
            {
                const auto wait = std::ranges::find(edited.items, Kind::kWait, &ntdlib::commands::Command::kind);
                SEQLIB_REQUIRE(wait != edited.items.end());
                edited.items.insert(wait + 1, 8, *wait);
            });
    }
    const format::DsImageExport grown = format::ExportDsImage(more);
    XSFLIB_CHECK(grown.image != image && grown.stale.empty());
    const ntdlib::ntr::Rom rom = ntdlib::ntr::Rom::Parse(grown.image);
    XSFLIB_CHECK(rom.FileData(ids[0]).size() > song.size() && rom.FileData(ids[2]).size() > effects.size());
    XSFLIB_CHECK(std::ranges::equal(rom.FileData(ids[1]), bank) && std::ranges::equal(rom.FileData(ids[3]), after));
    const File again = format::ImportDseImage(grown.image);
    SEQLIB_REQUIRE(again.sequences.size() == 3);
    XSFLIB_CHECK(*again.sequences[0] == *more.sequences[0] && *again.sequences[1] == *file.sequences[1] &&
                 *again.sequences[2] == *more.sequences[2]);
    XSFLIB_CHECK(format::ExportDsImage(again).image == grown.image);
}

// An import refuses an image without DSE's files. The model form and a new file refuse DSE.
void TestDseImageRefusals()
{
    namespace nds_files = ntdlib::testing::nds_files;

    const DseImageFixture fixture = MakeDseImageFixture();
    const std::vector<uint8_t>& after = fixture.after;
    const File& file = fixture.file;

    // An image without such files, the model form and a cartridge's absence.
    nds_files::Image other;
    other.files = {{"extra/after.bin", after}};
    XSFLIB_CHECK(!format::HoldsDseFiles(nds_files::BuildImage(other).first));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::ImportDseImage(nds_files::BuildImage(other).first); }));
    XSFLIB_CHECK(Throws<xsflib::FormatError>([&] { format::InModelForm(file); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([] { format::NewFile(Engine::kDse); }));
}

// A GBA ROM of 32 MiB without free space after its data. A run of 32 KiB of 0xFF bytes before its end is a filler run.
// A song that outgrows its place has no room without the filler runs: the export refuses it, and LacksRoom says so.
// The file's choice puts it into the run 256 bytes in, and both forms keep the choice in NATV's flags and the filler
// line. The program plays the ROM with the filler runs whatever the choice (PlayedBytes). WithSongsOf uses the song as
// the file opened, and the export then returns the game's ROM unchanged. A file of another console takes no filler.
void TestGbaFiller()
{
    const std::vector<uint8_t> rom = GbaRom(0x38000, ntdlib::mp2k::kLargestRom);
    const File file = format::ImportGbaRom(rom);
    SEQLIB_REQUIRE(file.native && !file.native->gba_filler && file.sequences.size() == 2);
    XSFLIB_CHECK(!format::LacksRoom(file) && format::ExportArchive(file) == rom);

    File more = file;
    more.sequences[0].Edit([](Sequence& sequence)
                           { sequence.items.insert(sequence.items.begin() + 1, 20, sequence.items[0]); });
    XSFLIB_CHECK(format::MovedSongs(more) == std::vector<uint32_t>{0});
    XSFLIB_CHECK(format::LacksRoom(more));
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::ExportArchive(more); }).find("32 MiB") !=
                 std::string::npos);

    File filler = more;
    filler.native.Edit([](NativeArchive& native) { native.gba_filler = true; });
    XSFLIB_CHECK(format::LacksRoom(filler));
    const std::vector<uint8_t> exported = format::ExportArchive(filler);
    SEQLIB_REQUIRE(exported.size() == rom.size());
    XSFLIB_CHECK(format::PlayedBytes(more) == exported && format::PlayedBytes(filler) == exported);
    XSFLIB_CHECK(std::equal(exported.begin() + 0x40000, exported.end(), rom.begin() + 0x40000));
    XSFLIB_CHECK(
        std::all_of(exported.begin() + 0x38000, exported.begin() + 0x38100, [](uint8_t b) { return b == 0xFF; }));
    XSFLIB_CHECK(exported[0x38100] != 0xFF);

    // The import of the new ROM finds the song in the run, and plays its edit.
    const File again = format::ImportGbaRom(exported);
    SEQLIB_REQUIRE(again.sequences.size() == 2);
    const auto without_zeros = [](std::vector<Command> items)
    {
        while (!items.empty() && items.back().kind == Kind::kData &&
               std::all_of(items.back().bytes.begin(), items.back().bytes.end(), [](uint8_t b) { return b == 0; }))
        {
            items.pop_back();
        }

        return items;
    };
    XSFLIB_CHECK(without_zeros(again.sequences[1]->items) == without_zeros(more.sequences[0]->items));

    // Both forms keep the choice. The binary form stores it in bit 0 of the byte after NATV's kind.
    const std::vector<uint8_t> binary = format::Write(filler);
    const std::string text = format::WriteText(filler);
    XSFLIB_CHECK(format::Read(binary) == filler && format::ReadText(text) == filler);
    XSFLIB_CHECK(format::Read(format::Write(more)) == more);
    XSFLIB_CHECK(text.find("\nnative gba\n    game_size ") != std::string::npos &&
                 text.find("\n    filler\n") != std::string::npos);
    XSFLIB_CHECK(format::WriteText(more).find("filler") == std::string::npos);
    const std::vector<std::string> ids = ChunkIds(binary);
    const auto natv = static_cast<std::size_t>(std::find(ids.begin(), ids.end(), "NATV") - ids.begin());
    SEQLIB_REQUIRE(natv < ids.size());
    XSFLIB_CHECK(ChunkBytes(binary, natv)[0] == 5 && ChunkBytes(binary, natv)[1] == 1);
    XSFLIB_CHECK(ChunkBytes(format::Write(more), natv)[1] == 0);
    XSFLIB_CHECK(Patched(binary, natv, 1, 5).find("A later version of Hokuto saved it") != std::string::npos);

    // A filler line belongs to a GBA ROM, once, before the pieces.
    std::string twice = text;
    twice.insert(twice.find("    filler\n"), "    filler\n");
    XSFLIB_CHECK(TextError(twice).find("one filler line") != std::string::npos);
    std::string late = text;
    late.erase(late.find("    filler\n"), 11);
    late.insert(late.find("\n    sequence_file ") + 1, "    filler\n");
    XSFLIB_CHECK(TextError(late).find("comes before its pieces") != std::string::npos);
    std::string switch_text = format::WriteText(format::ImportSwitchArchive(Archive()));
    switch_text.insert(switch_text.find("\n", switch_text.find("native switch")) + 1, "    filler\n");
    XSFLIB_CHECK(TextError(switch_text).find("only a GBA ROM") != std::string::npos);
    File other = format::ImportSwitchArchive(Archive());
    other.native.Edit([](NativeArchive& native) { native.gba_filler = true; });
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(other); }));
    XSFLIB_CHECK(!format::LacksRoom(other));

    // The song as the file opened: the export returns the game's ROM unchanged.
    const File kept = format::WithSongsOf(more, file, std::vector<uint32_t>{0});
    XSFLIB_CHECK(!format::LacksRoom(kept) && format::ExportArchive(kept) == rom);
    XSFLIB_CHECK(*kept.sequences[1] == *more.sequences[1]);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WithSongsOf(other, file, {}); }));
}

// A GBA ROM with Metroid Fusion's sequencer. The notes of that sequencer read two bytes after their code (the read of a
// note's arguments that ntdlib's WriteMetroid writes). The first note of the song has a key and a velocity, and the
// next byte starts a note of the running status. The import keeps the count from ntdlib's cut, and the song contains
// both notes. The export returns the ROM unchanged, and refuses a note with an extra length. Both forms keep the count:
// the binary form in bit 1 of the byte after NATV's kind, and the text form in a note_arguments line. The file of a
// plain ROM has neither. The line belongs to a GBA ROM with MusicPlayer2000, once, before the pieces, with a count of 2
// or 3. With 3, the song's first note would read the next byte as its extra length, and the text form refuses the song.
// Bit 1 belongs to such a ROM too.
void TestGbaTwoByteNotes()
{
    namespace mp2k_cart = ntdlib::testing::mp2k_cart;
    using enum ntdlib::mp2k::Command;
    mp2k_cart::Cart cart;
    cart.WriteMetroid(16, true);
    const uint32_t voices = cart.VoiceGroup({{0, mp2k_cart::Cart::Voice(0, cart.Wave(mp2k_cart::Saw(), 13379, 0))}});
    cart.Song({mp2k_cart::Track().Cmd(kCmdVoice, 0).Note(24, 60, 100, 50).Wait(24).Fine()}, voices);
    const ntdlib::agb::Rom made = cart.ToRom();
    const std::vector<uint8_t> rom = hokuto::testing::RomBytes(made);
    const File file = format::ImportGbaRom(rom);
    const File plain = format::ImportGbaRom(GbaRom());
    File extra = file;
    extra.sequences[0].Edit([](Sequence& sequence) { sequence.items[1].value.value = 25 * ntdlib::commands::kOne; });

    const std::vector<uint8_t> binary = format::Write(file);
    const std::string text = format::WriteText(file);

    SEQLIB_REQUIRE(file.native && file.sequences.size() == 1);
    XSFLIB_CHECK(file.native->gba_note_arguments == 2 && plain.native->gba_note_arguments == 3);
    const std::vector<Command>& items = file.sequences[0]->items;
    XSFLIB_CHECK(std::count_if(items.begin(), items.end(), [](const Command& c) { return c.kind == Kind::kNote; }) ==
                 2);
    XSFLIB_CHECK(format::ExportArchive(file) == rom);
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::ExportArchive(extra); }));
    XSFLIB_CHECK(format::Read(binary) == file && format::ReadText(text) == file);
    XSFLIB_CHECK(text.find("\nnative gba\n    game_size ") != std::string::npos &&
                 text.find("\n    note_arguments 2\n") != std::string::npos);
    XSFLIB_CHECK(format::WriteText(plain).find("note_arguments") == std::string::npos);
    const std::vector<std::string> ids = ChunkIds(binary);
    const auto natv = static_cast<std::size_t>(std::find(ids.begin(), ids.end(), "NATV") - ids.begin());
    SEQLIB_REQUIRE(natv < ids.size());
    XSFLIB_CHECK(ChunkBytes(binary, natv)[0] == 5 && ChunkBytes(binary, natv)[1] == 2);
    const std::vector<uint8_t> plain_binary = format::Write(plain);
    XSFLIB_CHECK(ChunkBytes(plain_binary, natv)[1] == 0);

    std::string twice = text;
    twice.insert(twice.find("    note_arguments 2\n"), "    note_arguments 2\n");
    XSFLIB_CHECK(TextError(twice).find("one note_arguments line") != std::string::npos);
    std::string late = text;
    late.erase(late.find("    note_arguments 2\n"), 21);
    late.insert(late.find("\n    sequence_file ") + 1, "    note_arguments 2\n");
    XSFLIB_CHECK(TextError(late).find("comes before its pieces") != std::string::npos);
    std::string one = text;
    one.replace(one.find("note_arguments 2"), 16, "note_arguments 1");
    XSFLIB_CHECK(TextError(one).find("reads 2 or 3 argument bytes") != std::string::npos);
    std::string three = text;
    three.replace(three.find("note_arguments 2"), 16, "note_arguments 3");
    XSFLIB_CHECK(TextError(three).find("leaves out its arguments") != std::string::npos);
    std::string plain_three = format::WriteText(plain);
    plain_three.insert(plain_three.find("\n", plain_three.find("native gba")) + 1, "    note_arguments 3\n");
    XSFLIB_CHECK(format::ReadText(plain_three) == plain);
    std::string switch_text = format::WriteText(format::ImportSwitchArchive(Archive()));
    switch_text.insert(switch_text.find("\n", switch_text.find("native switch")) + 1, "    note_arguments 2\n");
    XSFLIB_CHECK(TextError(switch_text).find("only a GBA ROM with MusicPlayer2000") != std::string::npos);
    const std::vector<uint8_t> switch_binary = format::Write(format::ImportSwitchArchive(Archive()));
    const std::vector<std::string> switch_ids = ChunkIds(switch_binary);
    const auto switch_natv =
        static_cast<std::size_t>(std::find(switch_ids.begin(), switch_ids.end(), "NATV") - switch_ids.begin());
    SEQLIB_REQUIRE(switch_natv < switch_ids.size());
    XSFLIB_CHECK(
        Patched(switch_binary, switch_natv, 1, 2).starts_with("The file is damaged: the native archive holds"));
    File other = format::ImportSwitchArchive(Archive());
    other.native.Edit([](NativeArchive& native) { native.gba_note_arguments = 2; });
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::Write(other); }));
    XSFLIB_CHECK(Throws<std::invalid_argument>([&] { format::WriteText(other); }));
}

// The importers, and NewFile for each engine that it makes, give a native archive whose engine is the file engine, and
// whose family is the engine family. SetEngine sets each engine, keeps a driver of the same engine and replaces one of
// another. The binary form refuses the archive types 0 and 15, on each side of the engines' codes, and the text form
// refuses a type that names no engine. The binary writer refuses an archive on a driver's engine with no driver.
void TestEngineOfNative()
{
    std::vector<File> files = {MakeGbaRomFixture().file, format::ImportDsArchive(DsArchive()),
                               MakeDsImageFixture().file, MakeDseImageFixture().file,
                               format::ImportSwitchArchive(Archive())};
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        if (facts.creatable)
        {
            files.push_back(format::NewFile(facts.engine));
        }
    }

    for (const File& file : files)
    {
        SEQLIB_REQUIRE(file.native);
        XSFLIB_CHECK(file.native->engine == file.engine);
        XSFLIB_CHECK(format::FamilyOf(*file.native) == format::FactsOf(file.engine).family);
    }

    NativeArchive native;
    for (const format::EngineFacts& facts : format::kEngineFacts)
    {
        format::SetEngine(native, facts.engine);
        XSFLIB_CHECK(native.engine == facts.engine);
        XSFLIB_CHECK(FlavorOf(native) == facts.flavor.value_or(Flavor::kNx));
        XSFLIB_CHECK(native.driver.has_value() == format::IsDriverEngine(facts.engine));
    }
    format::SetEngine(native, Engine::kRare);
    native.driver->revision = 1;
    format::SetEngine(native, Engine::kRare);
    XSFLIB_CHECK(native.driver->revision == 1);
    format::SetEngine(native, Engine::kKrawall);
    XSFLIB_CHECK(native.engine == Engine::kKrawall && native.driver->revision == 0);

    const File ds = format::ImportDsArchive(DsArchive());
    const std::vector<uint8_t> binary = format::Write(ds);
    for (const int code : {0, 15})
    {
        XSFLIB_CHECK(Patched(binary, "NATV", 0, static_cast<uint8_t>(code)) ==
                     "The file contains an archive type (" + std::to_string(code) +
                         ") that this version of Hokuto doesn't support. A later version of Hokuto saved it.");
    }
    XSFLIB_CHECK(TextError(Replaced(format::WriteText(ds), "\nnative ds\n", "\nnative nes\n"))
                     .find("there's no native archive type named nes") != std::string::npos);

    File no_driver = ds;
    no_driver.engine = Engine::kKonami;
    no_driver.native.Edit([](NativeArchive& native) { native.engine = Engine::kKonami; });
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::Write(no_driver); }) ==
                 "the native archive names no driver on a driver's engine");
    XSFLIB_CHECK(ErrorOf<std::invalid_argument>([&] { format::WriteText(no_driver); }) ==
                 "the native archive names no driver on a driver's engine");
}

// The readers of both forms refuse a file whose ENGN chunk or engine line names another engine than the type of its
// native archive, on either side. A file whose two engines agree reads. The test patches a Switch archive to the Wii U
// engine, and a DS archive and a GBA ROM to the Switch engine.
void TestTwoEngines()
{
    const std::string refused = "This file names two different engines. It may be damaged.";
    for (const File& file :
         {format::ImportSwitchArchive(Archive()), format::ImportDsArchive(DsArchive()), MakeGbaRomFixture().file})
    {
        const Engine other = file.engine == Engine::kSwitch ? Engine::kWiiU : Engine::kSwitch;
        const std::vector<uint8_t> binary = format::Write(file);
        XSFLIB_CHECK(Patched(binary, "ENGN", 0, format::CodeOf(file.engine)).empty());
        XSFLIB_CHECK(Patched(binary, "ENGN", 0, format::CodeOf(other)) == refused);
        XSFLIB_CHECK(Patched(binary, "NATV", 0, format::CodeOf(other)) == refused);

        const std::string text = format::WriteText(file);
        const std::string name(format::NameOf(file.engine));
        const std::string other_name(format::NameOf(other));
        XSFLIB_CHECK(TextError(text).empty());
        XSFLIB_CHECK(TextError(Replaced(text, "\nengine " + name + "\n", "\nengine " + other_name + "\n")) == refused);
        // The native archive of a GBA ROM has lines that a Switch archive can't have. The first such line refuses the
        // file.
        const std::string native_error =
            TextError(Replaced(text, "\nnative " + name + "\n", "\nnative " + other_name + "\n"));
        XSFLIB_CHECK(file.engine == Engine::kGba ? native_error.ends_with("isn't valid here.")
                                                 : native_error == refused);
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("format_roms",
                                    []
                                    {
                                        TestGbaRomImport();
                                        TestGbaRomForms();
                                        TestGbaRomLongerSong();
                                        TestGbaRomRefusals();
                                        TestGbaTwoByteNotes();
                                        TestDsArchive();
                                        TestDsImageImport();
                                        TestDsImageLongerSequence();
                                        TestDsImageArchiveInContainer();
                                        TestDsImageDsi();
                                        TestDsImageRefusals();
                                        TestDseImageImport();
                                        TestDseImageForms();
                                        TestDseImageLongerSong();
                                        TestDseImageRefusals();
                                        TestGbaFiller();
                                        TestEngineOfNative();
                                        TestTwoEngines();
                                    });
}
