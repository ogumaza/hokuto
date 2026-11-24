// SPDX-License-Identifier: MIT

// Tests the archives of an input that a command of hok runs on (src/cli/hok_archives.h). A command that takes chosen
// sounds runs once, on the archive that --driver names or else on the first archive. A command that writes every
// sound runs once for each archive, with a folder for each. The design gives both rules for a ROM with two drivers.

#include "cli/hok_archives.h"

#include <array>
#include <optional>
#include <vector>

#include "xsflib/testing/check.h"

#include "format/engine.h"

namespace
{

using hokuto::cli::ArchiveRun;
using hokuto::cli::ArchiveRuns;
using hokuto::format::Engine;
using xsflib::testing::Check;

constexpr std::array kTwoDrivers = {Engine::kGba, Engine::kRd2};
constexpr std::array kOneDriver = {Engine::kRd2};

bool IsOneRun(const std::vector<ArchiveRun>& runs, std::optional<Engine> driver)
{
    return runs.size() == 1 && runs.front().driver == driver && !runs.front().engine_folder;
}

void TestChosenSounds()
{
    Check(IsOneRun(ArchiveRuns(kTwoDrivers, std::nullopt, true), std::nullopt),
          "--sound without --driver takes the first archive, once");
    Check(IsOneRun(ArchiveRuns(kTwoDrivers, Engine::kRd2, true), Engine::kRd2),
          "--sound with --driver takes the archive of that engine");
}

void TestEverySound()
{
    const std::vector<ArchiveRun> runs = ArchiveRuns(kTwoDrivers, std::nullopt, false);
    Check(runs.size() == 2 && runs[0].driver == Engine::kGba && runs[1].driver == Engine::kRd2 &&
              runs[0].engine_folder && runs[1].engine_folder,
          "every sound of two archives runs once for each archive, with a folder for each");
    Check(IsOneRun(ArchiveRuns(kTwoDrivers, Engine::kGba, false), Engine::kGba),
          "--driver chooses one archive for every sound");
    Check(IsOneRun(ArchiveRuns(kOneDriver, std::nullopt, false), std::nullopt),
          "an input of one archive runs once, into the folder that it's given");
    Check(IsOneRun(ArchiveRuns({}, std::nullopt, false), std::nullopt), "an input without archives runs once");
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("hok_archives",
                                    []
                                    {
                                        TestChosenSounds();
                                        TestEverySound();
                                    });
}
