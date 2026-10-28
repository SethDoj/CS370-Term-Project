// Unit tests for the pure part of src/sensors/ads1115.c: config word and volts.
#include "../src/sensors/ads1115.h"

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

int main(void)
{
    // Datasheet table 8: OS=1, MUX=1xx (AINx/GND), PGA, MODE=1, DR=100, COMP_QUE=11.
    CHECK(ads1115_config_word(0, ADS_PGA_4V096) == 0xC383);
    CHECK(ads1115_config_word(1, ADS_PGA_4V096) == 0xD383);
    CHECK(ads1115_config_word(2, ADS_PGA_4V096) == 0xE383);
    CHECK(ads1115_config_word(3, ADS_PGA_4V096) == 0xF383);
    CHECK(ads1115_config_word(0, ADS_PGA_6V144) == 0xC183);
    CHECK(ads1115_config_word(0, ADS_PGA_2V048) == 0xC583);

    CHECK(ads1115_to_volts(0, ADS_PGA_4V096) == 0.0);
    CHECK(fabs(ads1115_to_volts(32767, ADS_PGA_4V096) - 4.096 * 32767 / 32768) < 1e-9);
    CHECK(ads1115_to_volts(-32768, ADS_PGA_4V096) == -4.096);
    CHECK(fabs(ads1115_to_volts(13250, ADS_PGA_4V096) - 1.65625) < 1e-9);

    if (failures == 0)
        printf("test_ads1115: all checks passed\n");
    return failures != 0;
}
