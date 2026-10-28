// Sampler -> hub record: one fixed-size SOCK_SEQPACKET message per reading.
// Both ends run on the same host, so the struct is sent in native byte order;
// the CRC-32 catches anything mangled between seal and check.
#ifndef ORCHID_RECORD_H
#define ORCHID_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define RECORD_VERSION 1

enum sensor_id { SENSOR_CLIMATE = 1, SENSOR_SOIL = 2 };

enum record_status {
    REC_OK,
    REC_CRC_ERR, // sensor's own checksum failed
    REC_IO_ERR, // NACK, timeout, unplugged
    REC_PARTIAL, // soil: some of the reads failed
    REC_OUT_OF_RANGE, // set by the hub when validation fails
    REC_STATUS_COUNT,
};

// Field meaning per sensor:
//   climate: raw = {raw_t, raw_rh}, value = {temp_c, rh_pct}
//   soil:    raw = {median code (int16 bits), unused}, value = {volts, unused}, n_ok
// Field order leaves no padding, so every byte the CRC covers is defined.
struct record {
    uint8_t version;
    uint8_t sensor_id;
    uint8_t status;
    uint8_t n_ok;
    uint32_t src_seq; // per-sampler counter; gaps mean lost records
    int64_t sample_time_ns; // CLOCK_REALTIME when the reading was taken
    double value[2];
    uint16_t raw[2];
    uint32_t crc32; // over every byte before this field
};

// Set version and crc32. Call last, after every other field is filled.
void record_seal(struct record *r);

// Validate a received message and copy it to *out. Returns NULL if it is
// good, or a short reason ("bad_len", "bad_crc", ...) for the log.
const char *record_check(const void *buf, size_t len, struct record *out);

// "OK", "CRC_ERR", ... as written to the CSV status column.
const char *record_status_name(enum record_status s);

#endif
