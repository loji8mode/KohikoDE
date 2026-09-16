#include "AppDirWatcher.h"

#include "Xdg.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>

namespace Kohiko
{

namespace
{

// Enough for several events to be batched together (a package
// manager installing/updating several apps at once, say) without
// needing more than one read() per Poll() call in the common case -
// read() on inotify never returns a partial event, so any leftover
// bytes just get picked up by Poll()'s own loop calling read() again.
constexpr std::size_t kBufferSize = 4096;

// Every way a .desktop file actually changes inside a watched
// directory - see this class's header comment for why *which* one
// fired doesn't matter.
constexpr std::uint32_t kWatchMask =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;

}

AppDirWatcher::AppDirWatcher() = default;

AppDirWatcher::~AppDirWatcher()
{
    // Closing the inotify fd itself automatically removes every watch
    // still associated with it - no need to inotify_rm_watch() each
    // one individually first.
    if (m_inotifyFd >= 0)
        ::close(m_inotifyFd);
}

void AppDirWatcher::Initialize()
{
    m_inotifyFd = inotify_init1(IN_NONBLOCK);

    if (m_inotifyFd < 0)
        return; // see Available()'s comment - not fatal

    for (const std::filesystem::path& dir : Xdg::ApplicationDirs())
    {
        std::error_code error;

        if (!std::filesystem::is_directory(dir, error))
            continue; // doesn't exist (yet) - nothing to watch here

        int watch = inotify_add_watch(m_inotifyFd, dir.c_str(), kWatchMask);

        if (watch >= 0)
            m_watchDescriptors.push_back(watch);
    }
}

bool AppDirWatcher::Available() const
{
    return m_inotifyFd >= 0;
}

int AppDirWatcher::Fd() const
{
    return m_inotifyFd;
}

bool AppDirWatcher::Poll()
{
    if (m_inotifyFd < 0)
        return false;

    bool sawEvent = false;

    std::array<char, kBufferSize> buffer;

    while (true)
    {
        ssize_t bytesRead = ::read(m_inotifyFd, buffer.data(), buffer.size());

        if (bytesRead <= 0)
        {
            // EAGAIN/EWOULDBLOCK just means "nothing left queued right
            // now" (m_inotifyFd is non-blocking) - the ordinary, expected
            // way this loop ends, not a real error.
            break;
        }

        sawEvent = true;

        // Nothing further to parse out of the buffer - see this
        // class's header comment on why *which* event/file doesn't
        // matter here, only that at least one arrived.
    }

    return sawEvent;
}

}
