// Drives AppInstanceLock::LaunchOrRaise() the way a tray widget does -
// from ONE long-lived process, several times at given offsets - not part of Kohiko.
//
//   launch_or_raise_probe --id ID --exe PATH --log FILE [--startup-ms N] [--stay-ms M] --clicks "0 120 250"
//
// The application is started with the probe app's own flags.

#include "AppInstanceLock.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace Kohiko;

int main(int argc, char** argv)
{
    std::string id, exe, log, clicks = "0", startup = "0", stay = "2000";

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--id") && i + 1 < argc) id = argv[++i];
        else if (!std::strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!std::strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!std::strcmp(argv[i], "--clicks") && i + 1 < argc) clicks = argv[++i];
        else if (!std::strcmp(argv[i], "--startup-ms") && i + 1 < argc) startup = argv[++i];
        else if (!std::strcmp(argv[i], "--stay-ms") && i + 1 < argc) stay = argv[++i];
    }

    // LaunchOrRaise execs a bare executable name; give it the probe's
    // arguments through a tiny wrapper script generated next to the log.
    std::string wrapper = log + ".launch.sh";
    {
        FILE* f = std::fopen(wrapper.c_str(), "w");
        std::fprintf(f, "#!/bin/sh\nexec \"%s\" --id %s --log \"%s\" --startup-ms %s --stay-ms %s\n",
            exe.c_str(), id.c_str(), log.c_str(), startup.c_str(), stay.c_str());
        std::fclose(f);
        chmod(wrapper.c_str(), 0755);
    }

    std::istringstream stream(clicks);
    int offset, last = 0;

    while (stream >> offset)
    {
        usleep(static_cast<useconds_t>(offset - last) * 1000);
        last = offset;
        AppInstanceLock::LaunchOrRaise(id, wrapper);
    }

    // Stay alive a moment so the children we started are still our
    // children (a tray widget is long-lived) while the caller observes.
    usleep(1200 * 1000);
    return 0;
}
