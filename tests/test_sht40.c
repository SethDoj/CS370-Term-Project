// Unit tests for the pure part of src/sensors/sht40.c: frame CRC and conversion.
#include "../src/common/crc.h"
#include "../src/sensors/sht40.h"

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

// Build a valid frame for the two raw words (crc8 is checked in test_crc).
static void make_frame(uint8_t f[6], uint16_t t, uint16_t rh)
{
    f[0] = (uint8_t)(t >> 8);
    f[1] = (uint8_t)t;
    f[2] = crc8_sensirion(f, 2);
    f[3] = (uint8_t)(rh >> 8);
    f[4] = (uint8_t)rh;
    f[5] = crc8_sensirion(f + 3, 2);
}

static int near(double a, double b)
{
    return fabs(a - b) < 0.005;
}

int main(void)
{
    uint8_t f[6];
    struct sht40_reading rd;

    // Datasheet word with known CRC: 0xBEEF -> 0x92.
    const uint8_t known[6] = { 0xBE, 0xEF, 0x92, 0xBE, 0xEF, 0x92 };
    CHECK(sht40_parse(known, &rd) == REC_OK);
    CHECK(rd.raw_t == 0xBEEF && rd.raw_rh == 0xBEEF);
    CHECK(near(rd.temp_c, -45.0 + 175.0 * 0xBEEF / 65535.0));

    // 0x6666 -> 25.00 C; 0x8000 -> 56.50 %RH.
    make_frame(f, 0x6666, 0x8000);
    CHECK(sht40_parse(f, &rd) == REC_OK);
    CHECK(near(rd.temp_c, 25.0));
    CHECK(near(rd.rh_pct, 56.50));

    // Extremes of the temperature formula.
    make_frame(f, 0x0000, 0x8000);
    CHECK(sht40_parse(f, &rd) == REC_OK && near(rd.temp_c, -45.0));
    make_frame(f, 0xFFFF, 0x8000);
    CHECK(sht40_parse(f, &rd) == REC_OK && near(rd.temp_c, 130.0));

    // RH clamps to 0..100.
    make_frame(f, 0x6666, 0x0000);
    CHECK(sht40_parse(f, &rd) == REC_OK && rd.rh_pct == 0.0);
    make_frame(f, 0x6666, 0xFFFF);
    CHECK(sht40_parse(f, &rd) == REC_OK && rd.rh_pct == 100.0);

    // A bad CRC on either word is CRC_ERR; raw words are still reported.
    make_frame(f, 0x6666, 0x8000);
    f[2] ^= 0x01;
    CHECK(sht40_parse(f, &rd) == REC_CRC_ERR);
    CHECK(rd.raw_t == 0x6666 && rd.temp_c == 0.0);
    make_frame(f, 0x6666, 0x8000);
    f[4] ^= 0x80; // data bit, not the CRC byte
    CHECK(sht40_parse(f, &rd) == REC_CRC_ERR);

    if (failures == 0)
        printf("test_sht40: all checks passed\n");
    return failures != 0;
}
