// Unit tests for src/hub/hub_core.c: routing, sequence stamping, validation,
// rejection, and SOCK_SEQPACKET message boundaries.
#include "../src/common/csvlog.h"
#include "../src/common/log.h"
#include "../src/common/record.h"
#include "../src/hub/hub_core.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

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

// Return a pointer to line idx (0 = header) of buf, or NULL.
static const char *line_at(const char *buf, int idx)
{
    for (; idx > 0 && buf; idx--) {
        buf = strchr(buf, '\n');
        if (buf)
            buf++;
    }
    return (buf && *buf) ? buf : NULL;
}

static size_t line_len(const char *l)
{
    const char *nl = strchr(l, '\n');
    return nl ? (size_t)(nl - l + 1) : strlen(l);
}

// The text after the timestamp and before the CRC: ",hub_seq,...,last".
static int body_is(const char *l, const char *want)
{
    const char *c = strchr(l, ',');
    size_t len = line_len(l);

    // Strip ",XXXXXXXX\n".
    return c && len > 10 && (size_t)(l + len - 10 - c) == strlen(want) &&
           memcmp(c, want, strlen(want)) == 0;
}

static struct record climate(uint32_t seq, enum record_status st, double t, double rh)
{
    struct record r = { .sensor_id = SENSOR_CLIMATE, .status = (uint8_t)st, .src_seq = seq };

    r.value[0] = t;
    r.value[1] = rh;
    r.raw[0] = 0x6666;
    r.raw[1] = 0x8000;
    record_seal(&r);
    return r;
}

static struct record soil(uint32_t seq, enum record_status st, int16_t code, double v, int n)
{
    struct record r = { .sensor_id = SENSOR_SOIL, .status = (uint8_t)st, .src_seq = seq };

    r.raw[0] = (uint16_t)code;
    r.value[0] = v;
    r.n_ok = (uint8_t)n;
    record_seal(&r);
    return r;
}

int main(void)
{
    char dir[] = "/tmp/orchid-test-hub-XXXXXX";
    char cpath[sizeof dir + 16], spath[sizeof dir + 16];
    char buf[4096];
    struct hub h;
    struct record r;
    int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    log_init(null_fd, "test");
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(cpath, sizeof cpath, "%s/%s", dir, HUB_CLIMATE_FILE);
    snprintf(spath, sizeof spath, "%s/%s", dir, HUB_SOIL_FILE);

    CHECK(hub_open(&h, dir) == 0);

    r = climate(1, REC_OK, 25.0, 56.5);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    r = soil(1, REC_OK, 13250, 1.65625, 16);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    r = climate(2, REC_CRC_ERR, 0, 0);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    // src_seq 3 lost in transit; hub_seq still counts only what it wrote.
    r = climate(4, REC_IO_ERR, 0, 0);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    r = climate(5, REC_OK, 25.0, NAN);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    r = soil(2, REC_PARTIAL, 13000, 1.625, 9);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    r = soil(3, REC_IO_ERR, 0, 0, 0);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    r = soil(4, REC_OK, 32767, 9.0, 16); // impossible voltage
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);

    // Corrupt or wrong-sized messages are rejected and not written.
    r = climate(6, REC_OK, 25.0, 56.5);
    ((unsigned char *)&r)[10] ^= 0x40;
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_REJECTED);
    CHECK(hub_handle_msg(&h, &r, 3) == HUB_REJECTED);

    hub_close(&h);

    slurp(cpath, buf, sizeof buf);
    const char *l;
    CHECK((l = line_at(buf, 0)) && strncmp(l, HUB_CLIMATE_HEADER "\n", line_len(l)) == 0);
    CHECK((l = line_at(buf, 1)) && body_is(l, ",1,1,OK,25.00,56.50,0x6666,0x8000"));
    CHECK((l = line_at(buf, 2)) && body_is(l, ",2,2,CRC_ERR,,,0x6666,0x8000"));
    CHECK((l = line_at(buf, 3)) && body_is(l, ",3,4,IO_ERR,,,,"));
    CHECK((l = line_at(buf, 4)) && body_is(l, ",4,5,OUT_OF_RANGE,25.00,nan,0x6666,0x8000"));
    CHECK(line_at(buf, 5) == NULL);
    for (int i = 1; i <= 4; i++)
        CHECK((l = line_at(buf, i)) && csvlog_row_ok(l, line_len(l)));
    // Timestamp shape: 2026-10-28T18:00:00.123Z
    CHECK((l = line_at(buf, 1)) && l[10] == 'T' && l[23] == 'Z' && l[24] == ',');

    slurp(spath, buf, sizeof buf);
    CHECK((l = line_at(buf, 0)) && strncmp(l, HUB_SOIL_HEADER "\n", line_len(l)) == 0);
    CHECK((l = line_at(buf, 1)) && body_is(l, ",1,1,OK,13250,1.6562,16"));
    CHECK((l = line_at(buf, 2)) && body_is(l, ",2,2,PARTIAL,13000,1.6250,9"));
    CHECK((l = line_at(buf, 3)) && body_is(l, ",3,3,IO_ERR,,,0"));
    CHECK((l = line_at(buf, 4)) && body_is(l, ",4,4,OUT_OF_RANGE,32767,9.0000,16"));
    CHECK(line_at(buf, 5) == NULL);
    for (int i = 1; i <= 4; i++)
        CHECK((l = line_at(buf, i)) && csvlog_row_ok(l, line_len(l)));

    // Reopening appends after existing rows without a second header.
    CHECK(hub_open(&h, dir) == 0);
    r = soil(5, REC_OK, 100, 0.0125, 16);
    CHECK(hub_handle_msg(&h, &r, sizeof r) == HUB_WRITTEN);
    hub_close(&h);
    slurp(spath, buf, sizeof buf);
    CHECK(strstr(buf + 1, "recv_time_utc") == NULL);
    CHECK((l = line_at(buf, 5)) && strstr(l, ",5,OK,100,0.0125,16,") != NULL);

    // SOCK_SEQPACKET keeps each record one message, even when queued back to back.
    int sp[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) == 0);
    struct record a = climate(7, REC_OK, 20.0, 40.0), b = soil(6, REC_OK, 1, 0.000125, 16);
    CHECK(send(sp[0], &a, sizeof a, 0) == (ssize_t)sizeof a);
    CHECK(send(sp[0], &b, sizeof b, 0) == (ssize_t)sizeof b);
    unsigned char rb[sizeof(struct record) + 1];
    struct record got;
    CHECK(recv(sp[1], rb, sizeof rb, 0) == (ssize_t)sizeof a);
    CHECK(record_check(rb, sizeof a, &got) == NULL && got.sensor_id == SENSOR_CLIMATE);
    CHECK(recv(sp[1], rb, sizeof rb, 0) == (ssize_t)sizeof b);
    CHECK(record_check(rb, sizeof b, &got) == NULL && got.sensor_id == SENSOR_SOIL);
    close(sp[0]);
    close(sp[1]);

    // A missing data directory fails cleanly with nothing left open.
    CHECK(hub_open(&h, "/nonexistent-orchid-dir") == -1);

    unlink(cpath);
    unlink(spath);
    rmdir(dir);
    close(null_fd);
    if (failures == 0)
        printf("test_hub: all checks passed\n");
    return failures != 0;
}
