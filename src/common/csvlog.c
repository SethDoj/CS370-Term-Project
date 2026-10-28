#include "csvlog.h"

#include "crc.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Write all of buf, retrying on EINTR and short writes. Returns 0 or -1.
static int write_all(int fd, const char *buf, size_t len)
{
    // Invariant: buf[0..len) is what remains to be written.
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_msg(LVL_ERROR, "event=csv_write_failed err=%s", strerror(errno));
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int sync_fd(int fd)
{
    if (fdatasync(fd) != 0) {
        log_msg(LVL_ERROR, "event=csv_sync_failed err=%s", strerror(errno));
        return -1;
    }
    return 0;
}

int csvlog_open(const char *path, const char *header)
{
    struct stat st;
    char last;
    // O_RDWR only so we can peek at the last byte for a torn row.
    int fd = open(path, O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0644);

    if (fd < 0) {
        log_msg(LVL_ERROR, "event=csv_open_failed path=%s err=%s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        log_msg(LVL_ERROR, "event=csv_stat_failed path=%s err=%s", path, strerror(errno));
        goto fail;
    }

    if (st.st_size == 0) {
        if (write_all(fd, header, strlen(header)) != 0 || write_all(fd, "\n", 1) != 0 ||
            sync_fd(fd) != 0)
            goto fail;
        return fd;
    }

    if (pread(fd, &last, 1, st.st_size - 1) != 1) {
        log_msg(LVL_ERROR, "event=csv_read_failed path=%s err=%s", path, strerror(errno));
        goto fail;
    }
    if (last != '\n') {
        log_msg(LVL_WARN, "event=csv_torn_row path=%s size=%lld", path, (long long)st.st_size);
        if (write_all(fd, "\n", 1) != 0 || sync_fd(fd) != 0)
            goto fail;
    }
    return fd;

fail:
    close(fd);
    return -1;
}

int csvlog_append(int fd, const char *fields)
{
    char row[CSV_ROW_MAX];
    size_t len = strlen(fields);

    // ",XXXXXXXX\n" is 10 bytes.
    if (len + 10 > sizeof row) {
        log_msg(LVL_ERROR, "event=csv_row_too_long len=%zu", len);
        return -1;
    }
    memcpy(row, fields, len);
    snprintf(row + len, sizeof row - len, ",%08X\n", (unsigned)crc32(fields, len));

    // One write() per row: a crash leaves at most one torn row, at the end.
    if (write_all(fd, row, len + 10) != 0)
        return -1;
    return sync_fd(fd);
}

int csvlog_row_ok(const char *row, size_t len)
{
    uint32_t want = 0;

    if (len > 0 && row[len - 1] == '\n')
        len--;
    // Need at least one field byte, a comma and 8 uppercase hex digits.
    if (len < 10 || row[len - 9] != ',')
        return 0;
    for (size_t i = len - 8; i < len; i++) {
        char c = row[i];
        if (c >= '0' && c <= '9')
            want = (want << 4) | (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F')
            want = (want << 4) | (uint32_t)(c - 'A' + 10);
        else
            return 0;
    }
    return want == crc32(row, len - 9);
}
