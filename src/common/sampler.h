// Event loop shared by every sampler: heartbeat, sample timer, sensor
// degrade/recover bookkeeping, and sending sealed records to the hub.
#ifndef ORCHID_SAMPLER_H
#define ORCHID_SAMPLER_H

#include "record.h"

#define HUB_SOCK_DEFAULT "orchid-hub.sock"

struct sampler {
    const char *sock_path;
    int interval_s; // seconds between samples, >= 1
    enum sensor_id sensor;

    // Open the device; called before a sample whenever it is not open.
    // Returns 0 or -1 (the sample is then reported as REC_IO_ERR).
    int (*open)(void *ctx);
    // Close the device; called after any REC_IO_ERR so the next sample reopens.
    void (*close)(void *ctx);
    // Take one reading: fill r->raw, r->value, r->n_ok and return its status.
    enum record_status (*read)(void *ctx, struct record *r);
    void *ctx;
};

// Run until SIGTERM/SIGINT. Returns the process exit code: EXIT_CLEAN on a
// signal, EXIT_CONFIG for a bad sock_path, EXIT_RESTART if the loop itself
// failed. The caller must have called child_init() and log_init() first.
int sampler_run(const struct sampler *s);

#endif
