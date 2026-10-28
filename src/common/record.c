#include "record.h"

#include "crc.h"

#include <string.h>

static_assert(sizeof(struct record) == 40, "struct record must have no padding");
static_assert(offsetof(struct record, crc32) == sizeof(struct record) - sizeof(uint32_t),
              "crc32 must be the last field");

static const char *const status_names[] = {
    [REC_OK] = "OK",
    [REC_CRC_ERR] = "CRC_ERR",
    [REC_IO_ERR] = "IO_ERR",
    [REC_PARTIAL] = "PARTIAL",
    [REC_OUT_OF_RANGE] = "OUT_OF_RANGE",
};

void record_seal(struct record *r)
{
    r->version = RECORD_VERSION;
    r->crc32 = crc32(r, offsetof(struct record, crc32));
}

const char *record_check(const void *buf, size_t len, struct record *out)
{
    if (len != sizeof *out)
        return "bad_len";
    memcpy(out, buf, sizeof *out);
    if (out->crc32 != crc32(out, offsetof(struct record, crc32)))
        return "bad_crc";
    if (out->version != RECORD_VERSION)
        return "bad_version";
    if (out->sensor_id != SENSOR_CLIMATE && out->sensor_id != SENSOR_SOIL)
        return "bad_sensor";
    if (out->status >= REC_STATUS_COUNT)
        return "bad_status";
    return NULL;
}

const char *record_status_name(enum record_status s)
{
    return (s < REC_STATUS_COUNT) ? status_names[s] : "?";
}
