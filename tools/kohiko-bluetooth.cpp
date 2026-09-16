#include "BluetoothWindow.h"

int main()
{
    Kohiko::BluetoothWindow window;

    if (!window.Initialize())
        return 0; // either display/setup failed, or another instance is already running and was raised instead

    window.Run();
    return 0;
}
