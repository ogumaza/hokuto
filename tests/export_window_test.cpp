// SPDX-License-Identifier: MIT

// Tests the notes of an export into a DS image (app/export_window.h) without a window. ntdlib's writer of the image
// names the hashes that an export leaves stale (ntdlib::ntr::WithFiles). The DSi's digest key updates the hashes of
// the digest, and no key updates the header's RSA signature, the whitelist's hashes or the signature for download play.

#include "app/export_warnings.h"
#include "app/export_window.h"

#include <optional>
#include <string>
#include <vector>

#include "ntdlib/ntr/rom_writer.h"
#include "xsflib/testing/check.h"

namespace
{

using hokuto::app::DsiNote;
using hokuto::app::SignatureNote;
using ntdlib::ntr::StaleHash;

const StaleHash kSignature{"the header's RSA signature", 0xF80, 1};
const StaleHash kHeaderHash{"the whitelist's hash of the header and the binaries", 0x33C, 1};
const StaleHash kOverlaysHash{"the whitelist's hash of the ARM9 overlays and the FAT", 0x350, 1};

// The note of a DSi image counts only the hashes of its digest. With a key, only the RSA signature stays out of date,
// and the note says that the export recomputed the digest.
void TestDsiNote()
{
    const std::vector<StaleHash> keyed = {kSignature};
    XSFLIB_CHECK(DsiNote(keyed) ==
                 "The image is DSi-enhanced. The export recomputed the hashes of its digest with the DSi digest key.");

    const std::vector<StaleHash> unkeyed = {{"the digest's master hash", 0x1B4, 1},
                                            {"the digest's sector hashes", 0x4000000, 13'000},
                                            {"the digest's block hashes", 0x4080000, 48},
                                            kSignature};
    XSFLIB_CHECK(DsiNote(unkeyed) ==
                 "The image is DSi-enhanced. The export leaves 13049 hashes of its digest out of "
                 "date. To update them, set the DSi digest key in Studio Setup.");
    XSFLIB_CHECK(DsiNote({{"the digest's master hash", 0x1B4, 1}}, true) ==
                 "The image is DSi-enhanced. The export leaves 1 hash of its digest out of date. To update it, choose "
                 "a DSi digest key file that Hokuto can read in Studio Setup.");
}

// The note of the hashes that no key updates lists them, on the DS and on the DSi. Without them, there's no note.
void TestSignatureNote()
{
    XSFLIB_CHECK(!SignatureNote({}, false));
    XSFLIB_CHECK(!SignatureNote({{"the digest's master hash", 0x1B4, 1}}, true));

    const std::vector<StaleHash> ds = {kSignature, kHeaderHash, kOverlaysHash};
    XSFLIB_CHECK(SignatureNote(ds, false) ==
                 "The export leaves these out of date, and no key can update them: the header's RSA signature, the "
                 "whitelist's hash of the header and the binaries, the whitelist's hash of the ARM9 overlays and the "
                 "FAT. A DSi or a 3DS may refuse to run the image.");
    XSFLIB_CHECK(SignatureNote({{"the digest's sector hashes", 0x4000000, 2}, kSignature}, true) ==
                 "The export leaves these out of date, and no key can update them: the header's RSA signature. A DSi "
                 "in DSi mode may refuse to run the image.");
    XSFLIB_CHECK(SignatureNote({{"the signature for download play", 0x800000, 1}}, false) ==
                 "The export leaves these out of date, and no key can update them: the signature for download play. "
                 "Download play from the image may fail.");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("export_window",
                                    []
                                    {
                                        TestDsiNote();
                                        TestSignatureNote();
                                    });
}
