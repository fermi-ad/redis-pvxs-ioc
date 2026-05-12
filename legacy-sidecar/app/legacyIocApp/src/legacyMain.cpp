#include <stddef.h>
#include <signal.h>
#include <unistd.h>

#include "epicsExit.h"
#include "epicsThread.h"
#include "iocsh.h"

namespace {

volatile sig_atomic_t shutdownRequested = 0;

void requestShutdown(int)
{
    shutdownRequested = 1;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc >= 2) {
        iocsh(argv[1]);
        epicsThreadSleep(.2);
    }

    if (isatty(STDIN_FILENO)) {
        iocsh(nullptr);
    } else {
        signal(SIGTERM, requestShutdown);
        signal(SIGINT, requestShutdown);

        while (!shutdownRequested) {
            epicsThreadSleep(1.0);
        }
    }

    epicsExit(0);
    return 0;
}
