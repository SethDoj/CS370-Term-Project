// Unit tests for src/common/csvlog.c: header once, row CRC, torn-row repair.
#include "../src/common/csvlog.h"
#include "../src/common/log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

// Read the whole file into buf (NUL-terminated). Returns its length.
static size_t slurp(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = (fd < 0) ? -1 : read(fd, buf, cap - 1);

    if (fd >= 0)
        close(fd);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
    return (size_t)n;
}

int main(void)
{
    char dir[] = "/tmp/orchid-test-csvlog-XXXXXX";
    char path[sizeof dir + 16];
    char buf[1024];
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    log_init(null_fd, "test");
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(path, sizeof path, "%s/t.csv", dir);

    // New file: header written once; reopening does not repeat it.
    int fd = csvlog_open(path, "a,b,crc32");
    CHECK(fd >= 0);
    CHECK(csvlog_append(fd, "1,2") == 0);
    close(fd);
    fd = csvlog_open(path, "a,b,crc32");
    CHECK(fd >= 0);
    CHECK(csvlog_append(fd, "3,4") == 0);
    close(fd);

    slurp(path, buf, sizeof buf);
    // CRC-32("1,2") and CRC-32("3,4"), checked independently with zlib.crc32.
    CHECK(strcmp(buf, "a,b,crc32\n1,2,2B0E6C9B\n3,4,C1E91DC0\n") == 0);

    // Each row verifies; one flipped byte does not.
    char *row = strchr(buf, '\n') + 1;
    char *end = strchr(row, '\n') + 1;
    CHECK(csvlog_row_ok(row, (size_t)(end - row)));
    CHECK(csvlog_row_ok(end, strlen(end)));
    row[0] = '9';
    CHECK(!csvlog_row_ok(row, (size_t)(end - row)));
    CHECK(!csvlog_row_ok("a,b,crc32", 9));
    CHECK(csvlog_row_ok("1,2,2B0E6C9B", 12));
    CHECK(!csvlog_row_ok("1,2,2b0e6c9b", 12)); // lowercase is not what we write
    CHECK(!csvlog_row_ok("", 0));

    // Torn last row (crash mid-write): reopen adds '\n' so the next row is whole.
    fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    CHECK(fd >= 0 && write(fd, "5,6,AB", 6) == 6);
    close(fd);
    fd = csvlog_open(path, "a,b,crc32");
    CHECK(fd >= 0);
    CHECK(csvlog_append(fd, "7,8") == 0);
    close(fd);
    slurp(path, buf, sizeof buf);
    char *torn = strstr(buf, "5,6,AB\n");
    CHECK(torn != NULL);
    if (torn) {
        char *next = torn + 7;
        CHECK(!csvlog_row_ok(torn, 7));
        CHECK(strncmp(next, "7,8,", 4) == 0 && csvlog_row_ok(next, strlen(next)));
    }

    // Too-long rows are refused, not cut.
    char big[CSV_ROW_MAX];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    fd = csvlog_open(path, "a,b,crc32");
    CHECK(csvlog_append(fd, big) == -1);
    close(fd);

    // Unopenable path fails cleanly.
    CHECK(csvlog_open("/nonexistent-dir/x.csv", "h") == -1);

    unlink(path);
    rmdir(dir);
    close(null_fd);
    if (failures == 0)
        printf("test_csvlog: all checks passed\n");
    return failures != 0;
}
