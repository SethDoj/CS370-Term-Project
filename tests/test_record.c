// Unit tests for src/common/record.c: seal/check round trip and every rejection.
#include "../src/common/crc.h"
#include "../src/common/record.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static struct record sample(void)
{
    struct record r = { 0 };

    r.sensor_id = SENSOR_CLIMATE;
    r.status = REC_OK;
    r.src_seq = 42;
    r.sample_time_ns = 1761674400123000000;
    r.value[0] = 23.41;
    r.value[1] = 55.2;
    r.raw[0] = 0x6A3C;
    r.raw[1] = 0x7B10;
    record_seal(&r);
    return r;
}

int main(void)
{
    struct record r = sample(), out;
    unsigned char buf[sizeof r + 1];

    CHECK(r.version == RECORD_VERSION);
    CHECK(record_check(&r, sizeof r, &out) == NULL);
    CHECK(memcmp(&r, &out, sizeof r) == 0);

    CHECK(strcmp(record_check(&r, sizeof r - 1, &out), "bad_len") == 0);
    memcpy(buf, &r, sizeof r);
    CHECK(strcmp(record_check(buf, sizeof buf, &out), "bad_len") == 0);

    // Every single-bit flip in the covered bytes is caught.
    int missed = 0;
    for (size_t i = 0; i < sizeof r; i++) {
        for (int b = 0; b < 8; b++) {
            memcpy(buf, &r, sizeof r);
            buf[i] ^= (unsigned char)(1u << b);
            if (record_check(buf, sizeof r, &out) == NULL)
                missed++;
        }
    }
    CHECK(missed == 0);

    // Correct CRC but bad contents: version, sensor, status.
    // record_seal() would reset version, so compute this CRC by hand.
    struct record bad = sample();
    bad.version = RECORD_VERSION + 1;
    bad.crc32 = crc32(&bad, offsetof(struct record, crc32));
    CHECK(strcmp(record_check(&bad, sizeof bad, &out), "bad_version") == 0);

    bad = sample();
    bad.sensor_id = 9;
    record_seal(&bad);
    CHECK(strcmp(record_check(&bad, sizeof bad, &out), "bad_sensor") == 0);

    bad = sample();
    bad.status = REC_STATUS_COUNT;
    record_seal(&bad);
    CHECK(strcmp(record_check(&bad, sizeof bad, &out), "bad_status") == 0);

    CHECK(strcmp(record_status_name(REC_OK), "OK") == 0);
    CHECK(strcmp(record_status_name(REC_IO_ERR), "IO_ERR") == 0);
    CHECK(strcmp(record_status_name(REC_STATUS_COUNT), "?") == 0);

    if (failures == 0)
        printf("test_record: all checks passed\n");
    return failures != 0;
}
