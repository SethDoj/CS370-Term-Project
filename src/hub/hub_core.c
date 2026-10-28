#include "hub_core.h"

#include "../common/csvlog.h"
#include "../common/log.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// Physical limits: values outside these mean a bad reading, not weather.
#define TEMP_MIN_C (-40.0) // SHT40 rated range
#define TEMP_MAX_C 125.0
#define RH_MIN 0.0
#define RH_MAX 100.0
#define SOIL_V_MIN (-0.3) // ADS1115 input limits at a 3.3 V supply
#define SOIL_V_MAX 3.6

static int open_one(const char *dir, const char *name, const char *header)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s/%s", dir, name);

    if (n < 0 || (size_t)n >= sizeof path) {
        log_msg(LVL_ERROR, "event=config_error why=path_too_long dir=%s", dir);
        return -1;
    }
    return csvlog_open(path, header);
}

int hub_open(struct hub *h, const char *dir)
{
    *h = (struct hub){ .fd = { -1, -1, -1 } };

    h->fd[SENSOR_CLIMATE] = open_one(dir, HUB_CLIMATE_FILE, HUB_CLIMATE_HEADER);
    if (h->fd[SENSOR_CLIMATE] < 0)
        goto fail;
    h->fd[SENSOR_SOIL] = open_one(dir, HUB_SOIL_FILE, HUB_SOIL_HEADER);
    if (h->fd[SENSOR_SOIL] < 0)
        goto fail;
    return 0;

fail:
    hub_close(h);
    return -1;
}

void hub_close(struct hub *h)
{
    for (int i = 0; i < 3; i++) {
        if (h->fd[i] >= 0)
            close(h->fd[i]);
        h->fd[i] = -1;
    }
}

static int in_range(double v, double lo, double hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

// Downgrade an OK/PARTIAL record whose values are physically impossible.
static void validate(struct record *r)
{
    if (r->status != REC_OK && r->status != REC_PARTIAL)
        return;
    int ok = (r->sensor_id == SENSOR_CLIMATE)
        ? in_range(r->value[0], TEMP_MIN_C, TEMP_MAX_C) && in_range(r->value[1], RH_MIN, RH_MAX)
        : in_range(r->value[0], SOIL_V_MIN, SOIL_V_MAX);
    if (!ok) {
        log_msg(LVL_WARN, "event=out_of_range sensor=%u src_seq=%u v0=%g v1=%g", r->sensor_id,
                r->src_seq, r->value[0], r->value[1]);
        r->status = REC_OUT_OF_RANGE;
    }
}

// "2026-10-28T18:00:00.123Z", same shape as log lines.
static void format_now(char *buf, size_t cap)
{
    struct timespec ts = { 0 };
    struct tm tm;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || gmtime_r(&ts.tv_sec, &tm) == NULL) {
        ts = (struct timespec){ 0 };
        gmtime_r(&ts.tv_sec, &tm);
    }
    size_t n = strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf + n, cap - n, ".%03ldZ", ts.tv_nsec / 1000000);
}

// Fill row with the CSV fields (no CRC) for r. Returns snprintf's result.
static int format_row(char *row, size_t cap, const char *when, uint32_t hub_seq,
                      const struct record *r)
{
    const char *st = record_status_name(r->status);
    int has_values = (r->status != REC_IO_ERR && r->status != REC_CRC_ERR);

    if (r->sensor_id == SENSOR_CLIMATE) {
        if (r->status == REC_IO_ERR)
            return snprintf(row, cap, "%s,%u,%u,%s,,,,", when, hub_seq, r->src_seq, st);
        if (!has_values) // CRC_ERR: raw words are kept for diagnosis
            return snprintf(row, cap, "%s,%u,%u,%s,,,0x%04X,0x%04X", when, hub_seq, r->src_seq,
                            st, r->raw[0], r->raw[1]);
        return snprintf(row, cap, "%s,%u,%u,%s,%.2f,%.2f,0x%04X,0x%04X", when, hub_seq,
                        r->src_seq, st, r->value[0], r->value[1], r->raw[0], r->raw[1]);
    }
    if (!has_values)
        return snprintf(row, cap, "%s,%u,%u,%s,,,%u", when, hub_seq, r->src_seq, st, r->n_ok);
    return snprintf(row, cap, "%s,%u,%u,%s,%d,%.4f,%u", when, hub_seq, r->src_seq, st,
                    (int16_t)r->raw[0], r->value[0], r->n_ok);
}

int hub_handle_msg(struct hub *h, const void *buf, size_t len)
{
    struct record r;
    char when[32];
    char row[CSV_ROW_MAX];
    const char *why = record_check(buf, len, &r);

    if (why != NULL) {
        log_msg(LVL_WARN, "event=record_rejected why=%s len=%zu", why, len);
        return HUB_REJECTED;
    }
    validate(&r);
    format_now(when, sizeof when);

    uint32_t seq = h->seq[r.sensor_id] + 1;
    int n = format_row(row, sizeof row, when, seq, &r);
    if (n < 0 || (size_t)n >= sizeof row) {
        log_msg(LVL_ERROR, "event=row_format_failed sensor=%u", r.sensor_id);
        return HUB_STORE_FAILED;
    }
    if (csvlog_append(h->fd[r.sensor_id], row) != 0)
        return HUB_STORE_FAILED;
    h->seq[r.sensor_id] = seq;
    return HUB_WRITTEN;
}
