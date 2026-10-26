# Design Document – Orchid Sitter (A B Lopez Alvarado, Seth Glasser)

## 1. Architecture

```
  SHT40 temp/RH        VEML7700 light        Capacitive soil probe (analog)
  I2C 0x44             I2C 0x10                     |
      |                    |                  ADC: MCP3008 (SPI)  [to be purchased; see 1.4]
      |                    |                        |
 ==== kernel space: stock i2c-dev (/dev/i2c-1) and spidev (/dev/spidev0.0); no custom driver ====
      |                    |                        |
 climate_sampler      light_sampler           soil_sampler            one process per sensor,
 1 read / 30 s        1 read / 60 s           16 reads -> median,     single-threaded
                                              1 rec / 60 s
      |                    |                        |
      |  record: ~32 B, 2/min    1/min              |  1/min
      |  heartbeat: 1 / 10 s from every sampler     |
      \____________________|________________________/
                           |  Unix domain socket, SOCK_SEQPACKET
                           v
   +----------------------------------------------------------------+
   | hub  (single-threaded epoll loop: sockets, timerfd, signalfd)  |
   |   validate range, stamp time + sequence number                 |
   |     |-- append, ~4 rec/min --> storage layer (D)               |
   |     |       CRC32 append-only log, one segment per day,        |
   |     |       ~150 KB/day; state.bin via tmp + fsync + rename    |
   |     |-- every 5 min --> analysis engine (our code)             |
   |             watering-event detection, drying rate vs. VPD,     |
   |             "days until dry" forecast with stated reason       |
   +----------------------------------------------------------------+
                           |  status snapshot, 1 / 10 s (read-only)
                           v
                 interface: LAN status page / CLI

 supervisor (root of the process tree)
   fork/exec of every process above; SIGCHLD -> restart with exponential backoff
   no heartbeat for > 30 s -> SIGKILL + restart
   hourly log line: per-process liveness, RSS, cumulative record/fault counts
```

### 1.1 Processes and threads

| Process | Role | Threads |
|---|---|---|
| supervisor | Starts, watches and restarts every other process; writes the hourly heartbeat log | 1 |
| climate_sampler | Reads the SHT40 (temperature, relative humidity) | 1 |
| light_sampler | Reads the VEML7700 (lux); optional sensor | 1 |
| soil_sampler | Reads the soil probe through the ADC | 1 |
| hub | Aggregates records, sole writer to storage, runs analysis | 1 |
| interface | Serves current status and recommendation on the LAN | 1 |

No process uses threads. Each one is a single event loop, so every wake-up has a single cause
we can name (a socket readable, a timer fired, a signal arrived).

### 1.2 Why this shape: from the user's needs

- **The device must be awake for days unattended.** An orchid dries over 4–10 days; a device
  that silently stops recording puts the user back on calendar watering. Hence a supervisor
  that survives any child dying (mechanism E).
- **One failed sensor must not cost us the others.** Each sensor lives in its own process, so a
  hung I2C read or a crash in one sampler cannot stop the others. The hub marks that channel
  `DEGRADED` and the analysis hedges its advice (for example "no humidity data; forecast from
  soil moisture only").
- **A crash and an unplug are different failures.** A crashed sampler closes its socket and
  raises SIGCHLD; the supervisor restarts it. An unplugged sensor leaves its process alive and
  reporting `SENSOR_ERR` heartbeats; the system degrades and re-probes periodically instead of
  restarting in a loop.
- **The drying model needs history that survives a power cut.** The drying-rate estimate is
  built from days of readings. The hub is the only writer to a checksummed append-only log
  (mechanism D). On restart it scans the newest segment and truncates at the first torn or
  bad-CRC record, so we lose at most the record being written.
- **The sensors cooperate in one decision.** Temperature and RH give vapor-pressure deficit
  (VPD). The analysis fits soil drying rate against VPD and forecasts days until dry, which no
  single threshold on any one sensor can do.

### 1.3 IPC choice

Samplers talk to the hub over Unix domain sockets of type `SOCK_SEQPACKET`. Data rates are a
few small records per minute, so shared memory's speed buys nothing. Sockets give us what we do
need: a dead peer shows up immediately as EOF/`EPOLLHUP`, each record arrives as one message,
and no lock can be left held by a process that died mid-write.

### 1.4 Hardware gap and shared-bus risk

- The soil probe is analog and the Pi has no analog inputs, so an ADC is required and has not
  yet been purchased. We plan an MCP3008 on SPI rather than an ADS1115 on I2C, so the soil probe
  does not share a bus with the SHT40 and VEML7700.
- The SHT40 and VEML7700 share I2C bus 1. A bus lockup would affect both together despite
  their separate processes; this is recorded in the failure-mode table.
