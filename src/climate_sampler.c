// climate_sampler: reads the SHT40 and sends one record per interval to the hub.
#include "common/child.h"
#include "common/i2c.h"
#include "common/log.h"
#include "common/sampler.h"
#include "sensors/sht40.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct climate {
    const char *bus;
    uint8_t addr;
    int fd;
};

static int climate_open(void *ctx)
{
    struct climate *c = ctx;

    c->fd = i2c_open(c->bus, c->addr);
    if (c->fd < 0)
        return -1;
    // Clear any half-finished command left by a previous run or a glitch.
    if (sht40_soft_reset(c->fd) != 0) {
        i2c_close(c->fd);
        c->fd = -1;
        return -1;
    }
    return 0;
}

static void climate_close(void *ctx)
{
    struct climate *c = ctx;

    i2c_close(c->fd);
    c->fd = -1;
}

static enum record_status climate_read(void *ctx, struct record *r)
{
    struct climate *c = ctx;
    struct sht40_reading rd;
    enum record_status st = sht40_measure(c->fd, &rd);

    if (st == REC_IO_ERR)
        return st;
    r->raw[0] = rd.raw_t;
    r->raw[1] = rd.raw_rh;
    r->value[0] = rd.temp_c;
    r->value[1] = rd.rh_pct;
    return st;
}

// Parse a whole decimal or 0x-hex integer in [lo, hi]. Returns 0 or -1.
static int parse_long(const char *s, long lo, long hi, long *out)
{
    char *end;

    errno = 0;
    long v = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: climate_sampler [-b /dev/i2c-1] [-a 0x44] [-s %s] [-i 30]\n",
            HUB_SOCK_DEFAULT);
}

int main(int argc, char **argv)
{
    struct climate c = { .bus = "/dev/i2c-1", .addr = SHT40_ADDR, .fd = -1 };
    struct sampler s = {
        .sock_path = HUB_SOCK_DEFAULT,
        .interval_s = 30,
        .sensor = SENSOR_CLIMATE,
        .open = climate_open,
        .close = climate_close,
        .read = climate_read,
        .ctx = &c,
    };
    long v;
    int opt;

    log_init(2, "climate_sampler");
    while ((opt = getopt(argc, argv, "b:a:s:i:")) != -1) {
        switch (opt) {
        case 'b':
            c.bus = optarg;
            break;
        case 'a':
            if (parse_long(optarg, 0x03, 0x77, &v) != 0)
                goto bad_arg;
            c.addr = (uint8_t)v;
            break;
        case 's':
            s.sock_path = optarg;
            break;
        case 'i':
            if (parse_long(optarg, 1, 86400, &v) != 0)
                goto bad_arg;
            s.interval_s = (int)v;
            break;
        default:
            goto bad_arg;
        }
    }
    if (optind != argc)
        goto bad_arg;

    if (child_init() != 0)
        return EXIT_RESTART;
    log_msg(LVL_INFO, "event=config bus=%s addr=0x%02x", c.bus, c.addr);
    return sampler_run(&s);

bad_arg:
    log_msg(LVL_ERROR, "event=config_error why=bad_arguments");
    usage();
    return EXIT_CONFIG;
}
