#include "NetworkWindow.h"

int main()
{
    Kohiko::NetworkWindow window;

    if (!window.Initialize())
        return 0; // either display/setup failed, or another instance is already running and was raised instead

    window.Run();
    return 0;
}
