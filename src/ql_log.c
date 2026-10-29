/*
 * ql_log -- see ql_log.h.
 */
#define _GNU_SOURCE
#include "ql_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static int           log_fd = -1;
static char          log_tag[16] = "ql";
static ql_console_fn console;
static ql_clock_fn   clock_fn;

static void set_tag(const char *tag)
{
	snprintf(log_tag, sizeof log_tag, "%s", tag ? tag : "ql");
}

void ql_log_open_path(const char *tag, const char *path)
{
	set_tag(tag);
	if (log_fd >= 0 || !path)
		return;                 /* stay as we are / stay disabled */

	log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (log_fd < 0) {
		/* said on stderr: an unopenable log path is otherwise
		 * indistinguishable from logging working */
		char buf[512];
		int  n = snprintf(buf, sizeof(buf),
		                  "%s: cannot open %s (%s); logging disabled\n",
		                  log_tag, path, strerror(errno));

		/* Truncated is still worth saying: a path long enough to overflow this
		 * buffer is exactly the kind most likely to be the mistake. */
		if ((size_t)n >= sizeof(buf)) {
			n = (int)sizeof(buf) - 1;
			buf[n - 1] = '\n';
		}
		if (n > 0 && write(2, buf, (size_t)n) < 0) {
			/* nothing sensible to do */
		}
	}
}

/* The "off" spellings let a variable inherited from a wrapper script or
 * unit file be overridden without editing it. */
void ql_log_open(const char *tag, const char *env)
{
	const char *path = env ? getenv(env) : NULL;

	if (path && (!path[0] || !strcasecmp(path, "0") || !strcasecmp(path, "off") ||
	             !strcasecmp(path, "none")))
		path = NULL;
	ql_log_open_path(tag, path);
}

void ql_log_close(void)
{
	if (log_fd >= 0) {
		close(log_fd);
		log_fd = -1;
	}
}

void ql_log_set_console(ql_console_fn fn)
{
	console = fn;
}

void ql_log_set_clock(ql_clock_fn fn)
{
	clock_fn = fn;
}

/* One line to the file: "[yyyy-mm-dd hh:mm:ss.mmm] tag: text\n", the game
 * time in the brackets too once there is a clock.  Never fails loudly. */
static void file_line(const char *text)
{
	char            buf[1100];
	int             n;
	struct timespec ts;
	struct tm       tm;
	char            game[16] = "";

	if (log_fd < 0)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	if (clock_fn)
		snprintf(game, sizeof game, " %d", clock_fn());
	n = snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03ld%s] %s: %s\n",
	             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
	             game, log_tag, text);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
		buf[n - 1] = '\n';
	}
	if (write(log_fd, buf, (size_t)n) < 0) {
		/* nothing sensible to do; never take the server down over a log line */
	}
}

/* One line to the console (or stderr), the text always as a %s argument. */
static void console_line(const char *prefix, const char *text)
{
	char safe[1024];
	size_t i;

	for (i = 0; i + 1 < sizeof safe && text[i]; i++)
		safe[i] = text[i] == '%' ? '.' : text[i];
	safe[i] = 0;
	text = safe;
	if (console) {
		console("%s%s\n", prefix, text);
	} else {
		char buf[1100];
		int  n = snprintf(buf, sizeof(buf), "%s%s\n", prefix, text);

		if (n < 0)
			return;
		if ((size_t)n >= sizeof(buf)) {
			n = (int)sizeof(buf) - 1;
			buf[n - 1] = '\n';
		}
		if (write(2, buf, (size_t)n) < 0) {
			/* nothing sensible to do */
		}
	}
}

void ql_trace(const char *fmt, ...)
{
	char    line[1024];
	va_list ap;

	if (log_fd < 0)
		return;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	file_line(line);
}

void ql_notice(const char *fmt, ...)
{
	char    line[1024], prefix[sizeof log_tag + 2];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	snprintf(prefix, sizeof prefix, "%s: ", log_tag);
	console_line(prefix, line);
	file_line(line);
}

void ql_row(const char *fmt, ...)
{
	char    line[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	console_line("", line);
	file_line(line);
}
