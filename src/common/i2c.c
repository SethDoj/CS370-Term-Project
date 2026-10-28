#include "i2c.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int i2c_open(const char *bus_path, uint8_t addr)
{
    int fd = open(bus_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        log_msg(LVL_WARN, "event=i2c_open_failed bus=%s err=%s", bus_path, strerror(errno));
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, (unsigned long)addr) < 0) {
        log_msg(LVL_WARN, "event=i2c_bind_failed bus=%s addr=0x%02x err=%s", bus_path, addr,
                strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

// i2c-dev turns each read()/write() into one transaction, so a short count
// means the device stopped responding partway; we report it, not retry.
int i2c_write(int fd, const uint8_t *buf, size_t len)
{
    ssize_t n;

    do
        n = write(fd, buf, len);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        log_msg(LVL_WARN, "event=i2c_write_failed err=%s", strerror(errno));
        return -1;
    }
    if ((size_t)n != len) {
        log_msg(LVL_WARN, "event=i2c_write_short want=%zu got=%zd", len, n);
        return -1;
    }
    return 0;
}

int i2c_read(int fd, uint8_t *buf, size_t len)
{
    ssize_t n;

    do
        n = read(fd, buf, len);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        log_msg(LVL_WARN, "event=i2c_read_failed err=%s", strerror(errno));
        return -1;
    }
    if ((size_t)n != len) {
        log_msg(LVL_WARN, "event=i2c_read_short want=%zu got=%zd", len, n);
        return -1;
    }
    return 0;
}

void i2c_close(int fd)
{
    if (fd >= 0 && close(fd) != 0)
        log_msg(LVL_WARN, "event=i2c_close_failed err=%s", strerror(errno));
}
