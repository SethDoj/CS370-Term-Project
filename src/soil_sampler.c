// soil_sampler: reads the capacitive soil probe through the ADS1115 and sends
// the median of SOIL_READS conversions to the hub once per interval.
#include "common/child.h"
#include "common/i2c.h"
#include "common/log.h"
#include "common/sampler.h"
#include "sensors/ads1115.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SOIL_READS 16

struct soil {
    const char *bus;
    uint8_t addr;
    unsigned channel;
    int fd;
};

static int soil_open(void *ctx)
{
    struct soil *s = ctx;

    s->fd = i2c_open(s->bus, s->addr);
    return s->fd < 0 ? -1 : 0;
}

static void soil_close(void *ctx)
{
    struct soil *s = ctx;

    i2c_close(s->fd);
    s->fd = -1;
}

// Median of v[0..n), n >= 1. Sorts v in place (insertion sort: n <= 16).
static int16_t median(int16_t *v, int n)
{
    for (int i = 1; i < n; i++) {
        int16_t x = v[i];
        int j = i - 1;
        for (; j >= 0 && v[j] > x; j--)
            v[j + 1] = v[j];
        v[j + 1] = x;
    }
    if (n % 2)
        return v[n / 2];
    return (int16_t)(((int32_t)v[n / 2 - 1] + v[n / 2]) / 2);
}

static enum record_status soil_read(void *ctx, struct record *r)
{
    struct soil *s = ctx;
    int16_t codes[SOIL_READS];
    int n = 0;

    for (int i = 0; i < SOIL_READS; i++)
        if (ads1115_read_single(s->fd, s->channel, ADS1115_SOIL_PGA, &codes[n]) == 0)
            n++;
    r->n_ok = (uint8_t)n;
    if (n == 0)
        return REC_IO_ERR;

    int16_t m = median(codes, n);
    r->raw[0] = (uint16_t)m;
    r->value[0] = ads1115_to_volts(m, ADS1115_SOIL_PGA);
    return n == SOIL_READS ? REC_OK : REC_PARTIAL;
}

// Parse a whole decimal or 0x-hex integer in [lo, hi]. Returns 0 or -1.
static int parse_long(const char *str, long lo, long hi, long *out)
{
    char *end;

    errno = 0;
    long v = strtol(str, &end, 0);
    if (errno != 0 || end == str || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: soil_sampler [-b /dev/i2c-1] [-a 0x48] [-c 0] [-s %s] [-i 60]\n",
            HUB_SOCK_DEFAULT);
}

int main(int argc, char **argv)
{
    struct soil soil = { .bus = "/dev/i2c-1", .addr = ADS1115_ADDR, .channel = 0, .fd = -1 };
    struct sampler s = {
        .sock_path = HUB_SOCK_DEFAULT,
        .interval_s = 60,
        .sensor = SENSOR_SOIL,
        .open = soil_open,
        .close = soil_close,
        .read = soil_read,
        .ctx = &soil,
    };
    long v;
    int opt;

    log_init(2, "soil_sampler");
    while ((opt = getopt(argc, argv, "b:a:c:s:i:")) != -1) {
        switch (opt) {
        case 'b':
            soil.bus = optarg;
            break;
        case 'a':
            if (parse_long(optarg, 0x03, 0x77, &v) != 0)
                goto bad_arg;
            soil.addr = (uint8_t)v;
            break;
        case 'c':
            if (parse_long(optarg, 0, 3, &v) != 0)
                goto bad_arg;
            soil.channel = (unsigned)v;
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
    log_msg(LVL_INFO, "event=config bus=%s addr=0x%02x ch=%u reads=%d", soil.bus, soil.addr,
            soil.channel, SOIL_READS);
    return sampler_run(&s);

bad_arg:
    log_msg(LVL_ERROR, "event=config_error why=bad_arguments");
    usage();
    return EXIT_CONFIG;
}
