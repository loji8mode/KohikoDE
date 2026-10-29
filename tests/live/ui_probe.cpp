// Minimal UiWindow application for the live tests - not part of Kohiko.
//
// One full-window Button. What its click does is chosen on the command
// line, so a test can reproduce exactly the situations a real native
// control app (kohiko-audio/network/bluetooth) gets into:
//
//   ui_probe --stall-ms N     onClick performs a blocking Xlib round trip
//                             after sleeping N ms (the sleep stands in for
//                             any slow call that happens to be followed by
//                             an X round trip - an icon load, XGetImage,
//                             a font query...). Events that arrive during
//                             it are read off the socket by that round
//                             trip and parked in Xlib's own queue.
//   ui_probe --timer-stall-ms N
//                             a 300 ms interval timer (the way the audio
//                             level meter / a D-Bus or PipeWire fd
//                             callback runs) does the same sleep + round
//                             trip, ONCE, outside the X event loop - the
//                             case where nothing wakes poll() again
//                             afterwards. Prints "TIMER_STALL".
//   ui_probe --block-ms N     onClick simply blocks the thread for N ms
//                             (a synchronous D-Bus call): the baseline for
//                             "UI frozen while an operation runs".
//
// Prints "READY 0x<window>" once mapped and "CLICKED"/"EXIT" as it goes.
// No window manager is required: the window is created at 0,0.

#include "UiWindow.h"
#include "UiWidget.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <unistd.h>

using namespace Kohiko;

int main(int argc, char** argv)
{
    int stallMs = 0;
    int blockMs = 0;
    int timerStallMs = 0;

    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--stall-ms") && i + 1 < argc) stallMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--block-ms") && i + 1 < argc) blockMs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--timer-stall-ms") && i + 1 < argc) timerStallMs = std::atoi(argv[++i]);
    }

    UiWindow window;
    if (!window.Create("ui_probe", "ui_probe", 300, 200))
        return 1;

    auto button = std::make_unique<Button>();
    button->label = "go";
    button->primary = true;

    Display* display = window.GetDisplay();

    button->onClick = [&]
    {
        std::printf("CLICKED\n");
        std::fflush(stdout);

        if (stallMs > 0)
        {
            usleep(static_cast<useconds_t>(stallMs) * 1000);
            XSync(display, False);   // reads whatever arrived meanwhile into Xlib's queue
        }

        if (blockMs > 0)
            usleep(static_cast<useconds_t>(blockMs) * 1000);

        std::printf("CLICK_DONE\n");
        std::fflush(stdout);
    };

    window.SetRoot(std::move(button));

    if (timerStallMs > 0)
    {
        bool done = false;
        window.SetInterval(300, [&, timerStallMs]
        {
            if (done)
                return;
            done = true;
            std::printf("TIMER_STALL\n");
            std::fflush(stdout);
            usleep(static_cast<useconds_t>(timerStallMs) * 1000);
            XSync(display, False);
        });
    }

    std::printf("READY 0x%lx\n", window.GetWindow());
    std::fflush(stdout);

    window.Run();

    std::printf("EXIT\n");
    std::fflush(stdout);
    return 0;
}
