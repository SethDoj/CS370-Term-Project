// Unit tests for src/common/crc.c against published check values.
#include "../src/common/crc.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int main(void)
{
    // SHT4x datasheet example: CRC(0xBEEF) = 0x92.
    const uint8_t beef[] = { 0xBE, 0xEF };
    CHECK(crc8_sensirion(beef, 2) == 0x92);
    CHECK(crc8_sensirion(beef, 0) == 0xFF);

    // Standard CRC-32 check value.
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    CHECK(crc32("", 0) == 0);
    CHECK(crc32("123456789", 8) != 0xCBF43926u);

    if (failures == 0)
        printf("test_crc: all checks passed\n");
    return failures != 0;
}
