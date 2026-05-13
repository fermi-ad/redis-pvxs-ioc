#include <csignal>
#include <unistd.h>

#include "epicsExit.h"
#include "epicsThread.h"
#include "iocsh.h"

namespace {

void addShutdownSignals(sigset_t& signals)
{
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
}

void waitForShutdownSignal(const sigset_t& signals)
{
    int signalNumber = 0;
    sigwait(&signals, &signalNumber);
}

}  // namespace

int main(int argc, char* argv[])
{
    const bool interactive = isatty(STDIN_FILENO) != 0;
    sigset_t shutdownSignals;

    if (!interactive) {
        addShutdownSignals(shutdownSignals);
        sigprocmask(SIG_BLOCK, &shutdownSignals, nullptr);
    }

    if (argc >= 2) {
        iocsh(argv[1]);
        epicsThreadSleep(.2);
    }

    if (interactive) {
        iocsh(nullptr);
    } else {
        waitForShutdownSignal(shutdownSignals);
    }

    epicsExit(0);
    return 0;
}
