// One-line structured logging shared by every Orchid Sitter process.
// Line format: <ISO-8601 UTC, ms> <LEVEL> <component> key=value ...\n
#ifndef ORCHID_LOG_H
#define ORCHID_LOG_H

// Longest line written, including the trailing '\n'. Longer messages are cut
// and end in LOG_TRUNC_MARK, so a lost field is visible in the log.
#define LOG_LINE_MAX 512
#define LOG_TRUNC_MARK "...[truncated]"

enum log_level { LVL_DEBUG, LVL_INFO, LVL_WARN, LVL_ERROR };

// Send all later lines to fd, tagged with component (not copied; must outlive use).
void log_init(int fd, const char *component);

// Format and emit one line with a single write(), so lines from several
// processes sharing one O_APPEND file never interleave.
void log_msg(enum log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
