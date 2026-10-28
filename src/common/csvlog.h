// Append-only CSV files, one row per write(), each row ending in its own CRC-32.
// Row format: <fields>,<CRC-32 of <fields> as 8 uppercase hex digits>\n
#ifndef ORCHID_CSVLOG_H
#define ORCHID_CSVLOG_H

#include <stddef.h>

// Longest row written, including ",XXXXXXXX\n".
#define CSV_ROW_MAX 256

// Open path for appending, creating it if needed. A new (empty) file gets
// header plus "\n". If the file ends in a torn row (no final '\n'), a '\n' is
// appended first so the next row starts on its own line. Returns fd or -1.
int csvlog_open(const char *path, const char *header);

// Append fields plus its CRC as one row and fdatasync. fields must not contain
// '\n'. Returns 0, or -1 on error or if the row would exceed CSV_ROW_MAX.
int csvlog_append(int fd, const char *fields);

// Check one row (with or without its trailing '\n'). Returns 1 if its last
// field is the CRC-32 of everything before the last comma, else 0.
int csvlog_row_ok(const char *row, size_t len);

#endif
