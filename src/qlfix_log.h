#ifndef QLFIX_LOG_H
#define QLFIX_LOG_H

/*
 * qlfix's names for the log (ql_log.h).  Every line goes to the file named
 * by QLFIX_LOG, when there is one; the console is where they differ:
 *
 *   qlfix_notice()   console too, as "qlfix: ...": arming, the per-load
 *                    patch record, strikes and kicks, classifier changes
 *                    folded to one line per slot per LOG_INTERVAL.  Human
 *                    frequency, main thread.
 *   qlfix_row()      console too, without the prefix: the qlfix_status
 *                    table (rcon output travels in 1008-byte packets, so a
 *                    row stays short).
 *   qlfix_trace()    file only, never folded: the classifier changes
 *                    between console lines, as they happen, and the
 *                    near-the-line and held samples.  Anything per command
 *                    goes here, because minqlx hooks Com_Printf and routes
 *                    every console line through a Python dispatcher.
 *
 * Until engine_arm() hands Com_Printf over, console lines go to stderr.
 */
#include "ql_log.h"

#define qlfix_trace      ql_trace
#define qlfix_notice     ql_notice
#define qlfix_row        ql_row
#define qlfix_log_open() ql_log_open("qlfix", "QLFIX_LOG")

#endif
