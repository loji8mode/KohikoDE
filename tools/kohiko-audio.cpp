#include "AudioWindow.h"

int main()
{
    Kohiko::AudioWindow window;

    if (!window.Initialize())
        return 0; // either display/PipeWire setup failed, or another instance is already running and was raised instead

    window.Run();
    return 0;
}
