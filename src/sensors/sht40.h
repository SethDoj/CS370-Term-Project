// Sensirion SHT40 temperature / relative-humidity sensor on I2C (addr 0x44).
#ifndef ORCHID_SHT40_H
#define ORCHID_SHT40_H

#include "../common/record.h"

#include <stdint.h>

#define SHT40_ADDR 0x44

struct sht40_reading {
    uint16_t raw_t;
    uint16_t raw_rh;
    double temp_c;
    double rh_pct;
};

// Decode a 6-byte measurement frame {T_hi, T_lo, crc, RH_hi, RH_lo, crc}.
// The raw words are always filled. Returns REC_OK, or REC_CRC_ERR (and leaves
// temp_c/rh_pct at 0) if either word fails its CRC-8.
enum record_status sht40_parse(const uint8_t frame[6], struct sht40_reading *out);

// One high-precision measurement on an fd from i2c_open(). Returns REC_OK,
// REC_CRC_ERR or REC_IO_ERR.
enum record_status sht40_measure(int fd, struct sht40_reading *out);

// Soft reset (0x94) and wait for the sensor to come back. Returns 0 or -1.
int sht40_soft_reset(int fd);

#endif
