// SPDX-License-Identifier: MIT

// The conformance suite (tests/conformance/README.md). Each line of tests/conformance/hashes.txt names a file of the
// suite, a sequence sound and the seconds of its render, and gives the render's SHA-256 (conformance_render.h). Each
// render has to give its hash.

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/testing/check.h"

#include "conformance_render.h"
#include "format/text.h"

namespace
{

// The folder of the suite's files, from the build.
const std::filesystem::path kFolder = xsflib::PathFromUtf8(HOKUTO_CONFORMANCE_DIR);

// Each render of hashes.txt gives its hash.
void TestRenders()
{
    std::ifstream list(kFolder / "hashes.txt");
    SEQLIB_CHECK(list.good());
    std::string line;
    int renders = 0;
    while (std::getline(list, line))
    {
        std::istringstream fields(line);
        std::string name;
        uint32_t sound = 0;
        double seconds = 0.0;
        std::string hash;
        if (!(fields >> name >> sound >> seconds >> hash))
        {
            continue;
        }

        std::vector<uint8_t> bytes;
        if (!xsflib::ReadFile(kFolder / xsflib::PathFromUtf8(name), bytes))
        {
            std::printf("%s: can't read the file\n", name.c_str());
            SEQLIB_CHECK(false);
            continue;
        }
        try
        {
            const std::string rendered = hokuto::conformance::Sha256(
                hokuto::conformance::Render(hokuto::format::ReadEitherForm(bytes), sound, seconds));
            if (rendered != hash)
            {
                std::printf("%s, sound %u: the render's hash is %s, not %s\n", name.c_str(), sound, rendered.c_str(),
                            hash.c_str());
            }
            SEQLIB_CHECK(rendered == hash);
        }
        catch (const std::exception& error)
        {
            std::printf("%s, sound %u: %s\n", name.c_str(), sound, error.what());
            SEQLIB_CHECK(false);
        }
        renders++;
    }

    // Four consoles in strict mixing, and three in enhanced mixing: the Wii has no enhanced mixing.
    SEQLIB_CHECK(renders == 7);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("conformance", [] { TestRenders(); });
}
