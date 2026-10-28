// Checksums: the SHT40's on-wire CRC-8 and the CRC-32 we put on records and rows.
#ifndef ORCHID_CRC_H
#define ORCHID_CRC_H

#include <stddef.h>
#include <stdint.h>

// Sensirion CRC-8: poly 0x31, init 0xFF, no reflection, no final XOR.
uint8_t crc8_sensirion(const uint8_t *buf, size_t len);

// IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320, init and final XOR 0xFFFFFFFF).
uint32_t crc32(const void *buf, size_t len);

#endif
