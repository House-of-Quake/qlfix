/*
 * ql_log -- the log.
 *
 * Every line goes to the file when <TAG>_LOG names one (unset, "", "0",
 * "off" or "none" = no file): "[yyyy-mm-dd hh:mm:ss.mmm] <tag>: ...", and
 * once a game clock is handed over with ql_log_set_clock()
 * "[yyyy-mm-dd hh:mm:ss.mmm <ms>]": the server's own time, the one its
 * snapshots carry, so a line can be placed on the game's timeline without
 * aligning two clocks.  ql_notice() and ql_row() go to the console as
 * well, with and without the "<tag>: " prefix; ql_trace() does not.  Until
 * the caller has verified Com_Printf and handed it over with
 * ql_log_set_console(), console lines go to stderr instead.
 *
 * ql_trace() exists because minqlx hooks Com_Printf and routes every line
 * through a Python dispatcher: a per-command console line would cost far
 * more than the work it reports and could re-enter us.  Keep ql_notice()
 * and ql_row() off hot paths and on the main thread.
 *
 * Every symbol is hidden (-fvisibility=hidden), so it never clashes with
 * another preload's symbols.
 */
#ifndef QL_LOG_H
#define QL_LOG_H

typedef void (*ql_console_fn)(const char *fmt, ...);
typedef int  (*ql_clock_fn)(void);

/* tag: "qlfix".  env: the variable naming the file ("QLFIX_LOG"). */
void ql_log_open(const char *tag, const char *env);
/* Same, with the path given directly; NULL = no file. */
void ql_log_open_path(const char *tag, const char *path);
void ql_log_close(void);

/* Com_Printf once verified; NULL = stderr. */
void ql_log_set_console(ql_console_fn fn);
/* The game clock in ms (sv.time) for the file prefix; NULL = none. */
void ql_log_set_clock(ql_clock_fn fn);

void ql_trace(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));   /* file only */
void ql_notice(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));   /* file + "<tag>: " line on the console */
void ql_row(const char *fmt, ...)     __attribute__((format(printf, 1, 2)));   /* file + console without the prefix (table rows) */

#endif
