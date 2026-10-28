#include "sht40.h"

#include "../common/crc.h"
#include "../common/i2c.h"

#include <errno.h>
#include <time.h>

#define CMD_MEASURE_HIGH 0xFD
#define CMD_SOFT_RESET 0x94

// Datasheet maximums: high-precision measurement 8.3 ms, soft reset 1 ms.
#define MEASURE_WAIT_MS 10
#define RESET_WAIT_MS 2

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

enum record_status sht40_parse(const uint8_t frame[6], struct sht40_reading *out)
{
    out->raw_t = (uint16_t)(frame[0] << 8 | frame[1]);
    out->raw_rh = (uint16_t)(frame[3] << 8 | frame[4]);
    out->temp_c = 0;
    out->rh_pct = 0;

    if (crc8_sensirion(frame, 2) != frame[2] || crc8_sensirion(frame + 3, 2) != frame[5])
        return REC_CRC_ERR;

    out->temp_c = -45.0 + 175.0 * out->raw_t / 65535.0;
    // The formula can leave 0..100; the datasheet says to clamp.
    double rh = -6.0 + 125.0 * out->raw_rh / 65535.0;
    out->rh_pct = rh < 0.0 ? 0.0 : rh > 100.0 ? 100.0 : rh;
    return REC_OK;
}

enum record_status sht40_measure(int fd, struct sht40_reading *out)
{
    static const uint8_t cmd = CMD_MEASURE_HIGH;
    uint8_t frame[6];

    if (i2c_write(fd, &cmd, 1) != 0)
        return REC_IO_ERR;
    sleep_ms(MEASURE_WAIT_MS);
    if (i2c_read(fd, frame, sizeof frame) != 0)
        return REC_IO_ERR;
    return sht40_parse(frame, out);
}

int sht40_soft_reset(int fd)
{
    static const uint8_t cmd = CMD_SOFT_RESET;

    if (i2c_write(fd, &cmd, 1) != 0)
        return -1;
    sleep_ms(RESET_WAIT_MS);
    return 0;
}
