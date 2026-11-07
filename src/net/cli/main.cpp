// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
#include "net/cli/PrinterCli.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <iostream>

namespace {
std::atomic<bool> gCancel{false};

// First Ctrl-C: stop cleanly (an upload asks the printer to drop the partial
// file). No SA_RESTART, so a pending confirmation prompt is interrupted and
// counts as "no". Second Ctrl-C: quit at once.
extern "C" void onSigint(int) {
    if (gCancel.exchange(true)) _exit(130);
}
}  // namespace

int main(int argc, char** argv) {
    struct sigaction sa {};
    sa.sa_handler = onSigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    // A printer that drops a connection must give an error, not kill us.
    signal(SIGPIPE, SIG_IGN);
    return fruit::net::runPrinterCli(argc, argv, std::cout, std::cerr, std::cin, &gCancel);
}
