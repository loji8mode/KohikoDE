#include "RecoveryMode.h"

#include "Xdg.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace Kohiko
{

namespace
{

std::filesystem::path MarkerPath()
{
    return Xdg::DataDir() / "startup-in-progress";
}

}

bool RecoveryMode::NoteStartupAttempt()
{
    bool previousAttemptDidNotFinish = std::filesystem::exists(MarkerPath());

    // (Re)create the marker for this attempt, regardless of the
    // above - an empty file is enough, its mere existence is the
    // entire signal (see this class's header comment).
    std::ofstream marker(MarkerPath(), std::ios::trunc);

    return previousAttemptDidNotFinish;
}

void RecoveryMode::MarkStartupSucceeded()
{
    std::error_code error;
    std::filesystem::remove(MarkerPath(), error);
}

}
