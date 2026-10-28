#include "ads1115.h"

#include "../common/i2c.h"
#include "../common/log.h"

#include <errno.h>
#include <time.h>

#define REG_CONVERSION 0x00
#define REG_CONFIG 0x01

#define CFG_OS (1u << 15) // write: start conversion; read: 1 = idle (done)
#define CFG_MUX_SINGLE(ch) ((4u + (ch)) << 12) // AINx vs GND
#define CFG_PGA(p) ((unsigned)(p) << 9)
#define CFG_MODE_SINGLE (1u << 8)
#define CFG_DR_128SPS (4u << 5)
#define CFG_COMP_OFF 3u

// One conversion at 128 SPS takes ~7.8 ms; poll every 2 ms for up to 50 ms.
#define POLL_MS 2
#define POLL_TRIES 25

static const double full_scale[] = {
    [ADS_PGA_6V144] = 6.144, [ADS_PGA_4V096] = 4.096, [ADS_PGA_2V048] = 2.048,
    [ADS_PGA_1V024] = 1.024, [ADS_PGA_0V512] = 0.512, [ADS_PGA_0V256] = 0.256,
};

uint16_t ads1115_config_word(unsigned channel, enum ads1115_pga pga)
{
    return (uint16_t)(CFG_OS | CFG_MUX_SINGLE(channel & 3u) | CFG_PGA(pga) | CFG_MODE_SINGLE |
                      CFG_DR_128SPS | CFG_COMP_OFF);
}

double ads1115_to_volts(int16_t code, enum ads1115_pga pga)
{
    return code * full_scale[pga] / 32768.0;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

// Point at reg and read its 16-bit big-endian value.
static int read_reg(int fd, uint8_t reg, uint16_t *val)
{
    uint8_t b[2];

    if (i2c_write(fd, &reg, 1) != 0 || i2c_read(fd, b, 2) != 0)
        return -1;
    *val = (uint16_t)(b[0] << 8 | b[1]);
    return 0;
}

int ads1115_read_single(int fd, unsigned channel, enum ads1115_pga pga, int16_t *code)
{
    uint16_t cfg = ads1115_config_word(channel, pga);
    uint8_t start[3] = { REG_CONFIG, (uint8_t)(cfg >> 8), (uint8_t)cfg };
    uint16_t val;

    if (i2c_write(fd, start, sizeof start) != 0)
        return -1;
    for (int i = 0;; i++) {
        sleep_ms(POLL_MS);
        if (read_reg(fd, REG_CONFIG, &val) != 0)
            return -1;
        if (val & CFG_OS)
            break;
        if (i + 1 >= POLL_TRIES) {
            log_msg(LVL_WARN, "event=ads1115_timeout ch=%u", channel);
            return -1;
        }
    }
    if (read_reg(fd, REG_CONVERSION, &val) != 0)
        return -1;
    *code = (int16_t)val;
    return 0;
}
