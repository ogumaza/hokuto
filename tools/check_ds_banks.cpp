// SPDX-License-Identifier: MIT

// Checks the Bank Editor's edits of DS banks (src/edit/ds_banks.h) on the sound archives of DS cartridge images. The
// tool imports each sound archive of an image (format::DsArchivesIn) into a Hokuto file the way the program does
// (format::ImportDsImage). It lists every region that the Bank Editor shows (app::MakeDsBankView): the region of each
// instrument of one region, and each row of a drum set or a key split. A region whose bytes another place shows counts
// once, at the first place. It then makes four rounds of edits, each in one version of the file:
//
// - Envelopes: each region gets a new attack, decay, sustain and release.
// - Keys and pans: each region gets a root key 5 higher and a pan 17 further right, both wrapping at 128.
// - Waves and duties: a wave changes to the next wave in the slots of its bank, and a square wave to the next duty.
// - Kinds: a wave becomes a square wave at duty 3, a square wave noise, and noise the first wave in the slots of its
//   bank. The edit refuses a new kind for a region whose bytes an instrument of one region at another program also
//   plays, for a wave by its address and for a silent region.
//
// A value of a region past 127, or a duty past 7, stays as the archive stores it. After each round, the tool exports
// the archive and checks it:
//
// - The archive keeps its size, and only the edited regions' bytes differ. Each region's 10 bytes hold the new values.
//   A new kind is in the table's record of an instrument of one region, or in the byte before a typed region's values.
// - ntdlib's reader reads each edited region with the new values and kind, and the Bank Editor shows each bank of the
//   new archive that it showed before.
// - The file comes back from its binary form and its text form, and produces the same archive.
//
// A fifth round halves the sustain of each wave, square wave and noise whose sustain is from 64 to 127. A square wave,
// noise and a wave that loops can hold a note. ntdlib's model of the console plays a held note of up to 256 such
// regions of each archive at velocity 127, before and after the edit. The model plays each note the way the Bank
// Editor's keyboard plays it. The driver turns a sustain into a level through its table of the levels of squares
// (ntdlib::ntr::snd::tables::kDecibelSquare). Once the note holds steady, its level has to fall by the difference
// between the levels of the two sustains within 0.5 dB. The driver's envelope steps once a frame. The attack raises the
// note from -72.3 dB towards full level, and the decay lowers it to the sustain by a fixed step. The tool works out the
// frames that both notes take to reach their sustains, by the rules of ntdlib's driver. A note that takes longer than
// 3.15 s counts apart: the levels count from 3.4 s. A note holds steady when its levels from 3.4 s and from 3.9 s, each
// over 0.1 s, are within 0.1 dB of each other. The sound unit cuts each output sample to 10 bits, and the cut drops up
// to a step. That error lifts a quiet level. The tool counts apart a note whose new level would be less than 20 dB
// above the error. It also counts apart a wave that doesn't loop, a note that doesn't hold steady, one that ends, and
// one that reaches full scale before the edit. A region whose attack or decay is past 127, past the driver's tables,
// counts apart. So does a wave whose wave archive ntdlib can't read.
//
// A round of waves edits each wave of each wave archive's file (edit/ds_waves.h): three quarters of its rate, at least
// 256 Hz, and a loop from the word at the middle of its samples. The tool works out the parameters' bytes by the
// format's rules. The export has to hold them and no other change. ntdlib has to read the new values, each wave has to
// decode to its samples before the edit, and the file has to come back from both its forms. A wave of one word of
// samples counts apart, and so does one that ntdlib can't decode.
//
// A round of structure edits the banks' models (edit/ds_banks.h) in one version of the file, each bank's file once.
// Each wave, square wave and noise of one region becomes a key split of one run over every key. Each key split of fewer
// than 8 runs whose first run ends at key 3 or later splits that run at its middle, with the same values. ntdlib has to
// read each bank as the model, and the file has to come back from both its forms. Every 32nd sequence then plays for 3
// s on the console's model, and has to produce the samples that it produced before, sample for sample. A sequence whose
// files no longer fit the DS's memory counts apart. In another version, up to 64 bank files each take a copy of an
// instrument at a free program. The Bank Editor's keyboard plays the copy for 1 s, and it has to produce the samples of
// the instrument.
//
// A round of drops then makes the Bank Editor's edits of a dropped wave file, in one version of the file, on up to 64
// banks of other wave archives. A copy of the bytes of a wave that an instrument plays is added after the waves of the
// first wave archive of the bank. It makes an instrument at a free program, beside an instrument of the same values
// that plays the old wave. The two have to give the same samples. The instrument then uses the new wave in place of the
// old one, and has to play as it did. Every file but the bank file and the wave archive file has to keep its bytes.
//
// The tool prints a line for each failure and refusal, and a summary. It returns 1 when an edit produces other bytes
// than it should, the edit refuses an edit that the tool doesn't expect it to refuse, a read, a view, a save or a
// render fails, or a level falls by another amount.
//
// `--structure` runs only the round of structure, the copies and the drops.
//
// usage: hokuto_check_ds_banks [--structure] <image.nds>...

#include "check_ds_banks.h"

namespace hokuto::tools::ds_banks
{

sdat::Region RegionAt(const DsArchive& archive, const DsRegionPlace& place)
{
    const sdat::Archive& tables = archive.archive;
    const sdat::Bank bank = sdat::Bank::Parse(tables.File(tables.banks.at(place.bank)->file_id));
    const sdat::Instrument& instrument = bank.instruments.at(place.program);
    const bool typed = instrument.type == kDrumSet || instrument.type == kKeySplit;

    return typed ? instrument.regions.at(place.region) : instrument.region;
}

namespace
{

// Each region that the Bank Editor shows of the game's banks. Notes the banks that it shows in `game`.
void FindPlaces(Game& game, Totals& totals)
{
    const DsArchive& archive = *game.archive;
    for (const uint32_t bank : hokuto::song::DsBanks(archive))
    {
        hokuto::app::DsBankView view;
        sdat::Bank file;
        std::size_t start = 0;
        try
        {
            view = hokuto::app::MakeDsBankView(archive, bank);
            const uint32_t file_id = archive.archive.banks[bank]->file_id;
            file = sdat::Bank::Parse(archive.archive.File(file_id));
            start = archive.archive.files.at(file_id).offset;
        }
        catch (const std::exception& error)
        {
            std::printf("%s: %s can't be shown: %s\n", game.name.c_str(),
                        hokuto::song::DsBankName(archive, bank).c_str(), error.what());
            totals.unread++;
            continue;
        }
        game.banks.push_back(bank);
        totals.banks++;

        for (const hokuto::app::DsProgram& program : view.programs)
        {
            for (const hokuto::app::DsRegionRow& row : program.rows)
            {
                const hokuto::edit::DsRegionBytes bytes =
                    hokuto::edit::DsRegionBytesOf(file, program.program, row.region);
                bool shared = false;
                for (uint32_t other = 0; other < file.instruments.size(); ++other)
                {
                    const sdat::InstrumentType type = file.instruments[other].type;
                    shared = shared || (other != program.program && (type == kPcm || type == kPsg || type == kNoise) &&
                                        file.instruments[other].offset == bytes.values);
                }
                std::string name = view.name + ", program " + std::to_string(program.program);
                int key = 60;
                if (program.type == kDrumSet)
                {
                    key = row.first_key;
                    name += ", key " + hokuto::app::KeyName(key);
                }
                else if (program.type == kKeySplit)
                {
                    key = std::clamp(60, row.first_key, row.last_key);
                    name +=
                        ", keys " + hokuto::app::KeyName(row.first_key) + " to " + hokuto::app::KeyName(row.last_key);
                }
                game.places.try_emplace(start + bytes.values, Place{name,
                                                                    {bank, program.program, row.region},
                                                                    key,
                                                                    row.values,
                                                                    start + bytes.values,
                                                                    start + bytes.kind,
                                                                    shared,
                                                                    view.waves});
            }
        }
    }
    totals.regions += game.places.size();
}

void CheckImage(const std::filesystem::path& path, bool only_structure, Totals& totals)
{
    const std::string image_name = xsflib::Utf8(path);
    std::vector<uint8_t> image;
    if (!xsflib::ReadFile(path, image))
    {
        std::printf("%s: can't be read\n", image_name.c_str());
        totals.failed++;
        return;
    }

    try
    {
        const ntdlib::ntr::snd::Build build = hokuto::song::DsBuildOf(image);
        for (const hokuto::format::DsArchivePlace& where : hokuto::format::DsArchivesIn(image))
        {
            Game game;
            game.name = image_name + ": " + where.path + " at " + std::to_string(where.offset);
            game.file = hokuto::format::ImportDsImage(image, where);
            game.bytes = hokuto::format::ExportArchive(game.file);
            game.archive = hokuto::song::LoadDsArchive(game.bytes, build);
            game.starts = hokuto::format::NativePieceStarts(game.file);
            totals.archives++;
            if (!only_structure)
            {
                Waves(game, totals);
            }
            if (hokuto::format::DsArchiveSharesFile(game.file))
            {
                totals.files_shared++;
            }
            else
            {
                Structure(game, totals);
                Copies(game, totals);
                Drops(game, totals);
            }
            if (only_structure)
            {
                continue;
            }

            FindPlaces(game, totals);
            if (game.places.empty())
            {
                continue;
            }

            std::map<std::size_t, sdat::Region> envelopes = Envelopes(game, totals);
            Round(game, "envelopes", envelopes, {}, totals);
            std::map<std::size_t, sdat::Region> keys_and_pans = KeysAndPans(game, totals);
            Round(game, "keys and pans", keys_and_pans, {}, totals);
            std::map<std::size_t, sdat::Region> waves_and_duties = WavesAndDuties(game, totals);
            Round(game, "waves and duties", waves_and_duties, {}, totals);
            std::set<std::size_t> refusing;
            std::map<std::size_t, sdat::Region> kinds = Kinds(game, refusing);
            Round(game, "kinds", kinds, refusing, totals);
            Levels(game, totals);
        }
    }
    catch (const std::exception& error)
    {
        std::printf("%s: %s\n", image_name.c_str(), error.what());
        totals.failed++;
    }
}

} // namespace

} // namespace hokuto::tools::ds_banks

using hokuto::tools::ds_banks::CheckImage;
using hokuto::tools::ds_banks::Totals;

int main(int argc, char** argv)
{
    const std::vector<std::string> arguments = xsflib::console::Arguments(argc, argv);
    const bool only_structure = arguments.size() >= 2 && arguments[1] == "--structure";
    const std::size_t first = only_structure ? 2 : 1;
    if (arguments.size() <= first)
    {
        std::fprintf(stderr, "usage: hokuto_check_ds_banks [--structure] <image.nds>...\n");
        return 2;
    }

    Totals totals;
    for (std::size_t i = first; i < arguments.size(); i++)
    {
        CheckImage(xsflib::PathFromUtf8(arguments[i]), only_structure, totals);
    }

    std::printf(
        "%zu archives, %zu banks, %zu that can't be shown, %zu regions. %zu edits, %zu refused, %zu kinds kept "
        "as the edit refuses them, %zu values kept past their range. %zu levels measured, the worst %.3f dB "
        "off the sustains' levels. Apart: %zu waves that don't loop, %zu whose wave archive can't be read, %zu regions "
        "past the driver's tables, %zu notes that reach their sustains too late, %zu that don't hold steady, %zu that "
        "end, %zu too quiet, %zu clipped. %zu failures.\n",
        totals.archives, totals.banks, totals.unread, totals.regions, totals.edits, totals.refused, totals.kinds_kept,
        totals.kept, totals.levels, totals.worst, totals.ending, totals.unread_holds, totals.past_range, totals.slow,
        totals.unsteady, totals.ended, totals.quiet, totals.clipped, totals.failed);
    std::printf("Waves: %zu wave archives' files, %zu that can't be read, %zu edits, %zu waves apart.\n",
                totals.wave_files, totals.unread_waves, totals.wave_edits, totals.waves_apart);
    std::printf(
        "Structure: %zu bank files, %zu new key splits, %zu runs split, %zu archives that share their file. "
        "%zu sequences played the same, %zu silent, %zu too large for memory, %zu that can't be read. "
        "%zu copies played as their instruments, %zu of them silent, %zu that a note can't play. "
        "%zu dropped waves played as they should, %zu of them silent, %zu apart.\n",
        totals.structure_files, totals.key_splits, totals.split_runs, totals.files_shared, totals.same_sounds,
        totals.silent_sounds, totals.too_large, totals.unplayed, totals.copies, totals.copies_silent,
        totals.copies_apart, totals.drops, totals.drops_silent, totals.drops_apart);

    return totals.failed > 0 ? 1 : 0;
}
