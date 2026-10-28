// Hub record handling with no sockets: check a received message, validate its
// values, stamp receive time and a per-sensor sequence number, and append it
// as one CSV row to that sensor's file.
#ifndef ORCHID_HUB_CORE_H
#define ORCHID_HUB_CORE_H

#include "../common/record.h"

#include <stddef.h>
#include <stdint.h>

#define HUB_CLIMATE_FILE "climate.csv"
#define HUB_SOIL_FILE "soil.csv"

#define HUB_CLIMATE_HEADER "recv_time_utc,hub_seq,src_seq,status,temp_c,rh_pct,raw_t,raw_rh,crc32"
#define HUB_SOIL_HEADER "recv_time_utc,hub_seq,src_seq,status,raw_median,volts,n_ok,crc32"

struct hub {
    int fd[3]; // indexed by enum sensor_id; [0] unused
    uint32_t seq[3]; // last hub_seq written per sensor
};

// Open (creating if needed) the per-sensor CSV files in dir. Returns 0, or -1
// with nothing left open.
int hub_open(struct hub *h, const char *dir);

void hub_close(struct hub *h);

// Results of hub_handle_msg.
#define HUB_WRITTEN 0
#define HUB_REJECTED (-1) // bad record: logged and dropped
#define HUB_STORE_FAILED (-2) // good record, but the append failed (logged)

int hub_handle_msg(struct hub *h, const void *buf, size_t len);

#endif
