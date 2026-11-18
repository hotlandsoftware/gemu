#pragma once
#include <stdbool.h>

/*
 * Machine Manager: the window `gemu` opens when it is started with no
 * arguments in a graphical session (GTK builds only).  The user picks a
 * machine, its media, devices and display; Start runs that command line as
 * a separate gemu process, and the manager stays open for the next one.
 *
 * Returns false without showing anything when there is no graphical
 * session (or GTK can't open one) - the caller prints its usage text then -
 * and true once the window has been closed and every machine it started
 * has exited.  prog is argv[0], shown in the command-line preview.
 */
bool gemu_machine_manager_run(const char *prog);
