#include "LockRecovery.h"

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
    return Xdg::DataDir() / "session-locked";
}

}

bool LockRecovery::WasLockedAtLastExit()
{
    return std::filesystem::exists(MarkerPath());
}

void LockRecovery::NoteLocked()
{
    // An empty file is enough - its mere existence is the entire
    // signal, same as RecoveryMode's own marker.
    std::ofstream marker(MarkerPath(), std::ios::trunc);
}

void LockRecovery::NoteUnlocked()
{
    std::error_code error;
    std::filesystem::remove(MarkerPath(), error);
}

}
