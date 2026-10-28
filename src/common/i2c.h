// Thin wrapper over the stock i2c-dev driver (/dev/i2c-N). Every failure is
// logged with errno and returns -1; callers decide whether to degrade.
#ifndef ORCHID_I2C_H
#define ORCHID_I2C_H

#include <stddef.h>
#include <stdint.h>

// Open bus_path and bind it to the 7-bit address addr. Returns an fd or -1.
int i2c_open(const char *bus_path, uint8_t addr);

// Write or read exactly len bytes as one I2C transaction. Returns 0 or -1.
int i2c_write(int fd, const uint8_t *buf, size_t len);
int i2c_read(int fd, uint8_t *buf, size_t len);

// Close fd if it is open (>= 0). Safe to call with -1.
void i2c_close(int fd);

#endif
