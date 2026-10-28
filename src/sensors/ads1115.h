// TI ADS1115 16-bit ADC on I2C (addr 0x48), used single-shot, single-ended.
#ifndef ORCHID_ADS1115_H
#define ORCHID_ADS1115_H

#include <stdint.h>

#define ADS1115_ADDR 0x48

// Programmable gain: full-scale range of the input.
enum ads1115_pga {
    ADS_PGA_6V144,
    ADS_PGA_4V096,
    ADS_PGA_2V048,
    ADS_PGA_1V024,
    ADS_PGA_0V512,
    ADS_PGA_0V256,
};

// The range we read the soil probe at (its output stays under ~3 V).
#define ADS1115_SOIL_PGA ADS_PGA_4V096

// Config register value that starts one conversion of AIN<channel> vs GND
// at 128 SPS with the comparator off. channel must be 0..3.
uint16_t ads1115_config_word(unsigned channel, enum ads1115_pga pga);

// Convert a conversion-register code to volts at the given gain.
double ads1115_to_volts(int16_t code, enum ads1115_pga pga);

// Start a conversion, wait for it (up to ~50 ms), and read the result.
// Returns 0 and sets *code, or -1 on I/O error or timeout (logged).
int ads1115_read_single(int fd, unsigned channel, enum ads1115_pga pga, int16_t *code);

#endif
