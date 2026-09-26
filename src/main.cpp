#include "ui/App.h"
#include <csignal>

int main()
{
    // OpenVPN may close its management connection while Burrow writes to it.
    signal(SIGPIPE, SIG_IGN);
    burrow::BurrowApp app;
    app.Run();
    return 0;
}
