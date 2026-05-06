# canbus-monitor

**CAN Bus Monitor and Logger**, built on the
Linux [SocketCAN](https://www.kernel.org/doc/html/latest/networking/can.html)
subsystem. Captures CAN frames from any SocketCAN-compatible interface, writes
rotating ASC-format log files, and reports per-ID bus statistics.

**Project type:** Linux (project 1 of 3 in the rotation)  
**License:** GPL-3.0-or-later  
**Language:** C11 — no proprietary dependencies

---

## Features

| Feature | Details |
|---|---|
| SocketCAN reception | `AF_CAN / SOCK_RAW`, hardware + software timestamping |
| Kernel-level filtering | Up to 32 `id:mask` filters applied before user-space copy |
| ASC log format | Compatible with CANalyzer, `cantools`, `python-can` |
| Log rotation | Size-based rotation with configurable retention count |
| Lock-free logging | SPSC ring buffer decouples receive path from disk I/O |
| Background flush thread | Configurable flush interval; receive thread never blocks on disk |
| Graceful shutdown | `SIGINT`/`SIGTERM` → flush + exit; `SIGHUP` → rotate log |
| Per-ID statistics | Frame count, byte count, DLC mean, inter-frame interval (Welford) |
| Self-pipe signal handling | `poll()` event loop wakes immediately on signal — no busy-wait |
| Hardened build | `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion` |
| Tests | 41 unit tests via a Unity-compatible framework (all passing) |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        main thread                              │
│  poll(CAN socket fd, signal pipe fd)                            │
│       │                                                         │
│       ├─ CAN frame ──► can_socket_recv()                        │
│       │                 ├─► frame_logger_write()  (lock-free)   │
│       │                 ├─► stats_update()                      │
│       │                 └─► stdout (verbose mode)               │
│       └─ signal ──► set exit/rotate flag                        │
└────────────────────┬────────────────────────────────────────────┘
                     │  ring buffer (SPSC, C11 atomics)
┌────────────────────▼────────────────────────────────────────────┐
│               flush thread                                      │
│   rb_pop() ──► fwrite() ASC lines ──► rotate file if needed     │
└─────────────────────────────────────────────────────────────────┘
```

**Module overview:**

| Module | File | Responsibility |
|---|---|---|
| `can_socket` | `src/can_socket.c` | SocketCAN socket lifecycle, hardware timestamps, kernel filters |
| `frame_logger` | `src/frame_logger.c` | ASC log writing, file rotation, flush thread |
| `ring_buffer` | `src/ring_buffer.c` | Lock-free SPSC ring buffer (C11 atomics) |
| `signal_handler` | `src/signal_handler.c` | Self-pipe trick, `sigaction` installation |
| `statistics` | `src/statistics.c` | FNV-1a hash table, Welford online mean/variance |
| `cli_parser` | `src/cli_parser.c` | `getopt_long` argument parsing |
| `main` | `src/main.c` | `poll()` event loop, orchestration |

---

## Requirements

- Linux kernel ≥ 3.6 (SocketCAN, `SO_TIMESTAMPING`)
- GCC ≥ 9 or Clang ≥ 10 (C11, `_Atomic`)
- CMake ≥ 3.18
- A SocketCAN-compatible CAN controller **or** the `vcan` kernel module for testing

**Optional runtime tools** (from `can-utils`):
```bash
sudo apt-get install can-utils
```

---

## Building

```bash
# Clone the repo
git clone https://github.com/ahr2042/canbus-monitor.git
cd canbus-monitor

# Debug build (with AddressSanitizer + UBSan)
./scripts/build.sh debug

# Release build
./scripts/build.sh release

# Clean rebuild
./scripts/build.sh debug clean
```

The binary is placed at `build/canbus_monitor`.

---

## Running Tests

```bash
cd build
ctest --output-on-failure
```

Or run individual test executables directly:

```bash
./build/tests/test_ring_buffer
./build/tests/test_statistics
./build/tests/test_cli_parser
./build/tests/test_frame_logger
```

All 41 tests pass.

---

## Usage

### Set up a virtual CAN interface (for development)

```bash
sudo ./scripts/setup_vcan.sh up vcan0

# Send a test frame
cansend vcan0 1A0#01020304
```

### Run the monitor

```bash
# Monitor all frames, log to ./canbus_logs/
./build/canbus_monitor vcan0

# Log to /var/log/can, custom prefix, print stats on exit
./build/canbus_monitor -o /var/log/can -p vehicle --stats can0

# Accept only frames with ID 0x100..0x1FF (11-bit mask)
./build/canbus_monitor -f 100:700 vcan0

# Capture 5000 frames then exit
./build/canbus_monitor -n 5000 --stats vcan0

# Verbose (print every frame to stdout)
./build/canbus_monitor -v vcan0
```

### Signal handling

```bash
# Graceful shutdown
kill -SIGINT  $(pidof canbus_monitor)

# Rotate log without stopping
kill -SIGHUP  $(pidof canbus_monitor)
```

### Log format (ASC)

```
date Tue Apr 28 17:00:01 2026
base hex  timestamps absolute
no internal events logged
// canbus_monitor v1.0.0
    0.001234  1  1A0  Rx  d  4  01 02 03 04
    0.002501  1  0CF  Rx  d  8  FF FF 00 00 01 02 03 04
```

ASC files can be read by:
- [python-can](https://python-can.readthedocs.io/) — `can.ASCReader`
- [cantools](https://github.com/eerimoq/cantools)
- Vector CANalyzer / CANdb++

---

## Project Structure

```
canbus-monitor/
├── CMakeLists.txt          # Top-level build definition
├── LICENSE                 # GPL-3.0-or-later
├── README.md
├── include/
│   ├── can_socket.h
│   ├── cli_parser.h
│   ├── frame_logger.h
│   ├── ring_buffer.h
│   ├── signal_handler.h
│   └── statistics.h
├── src/
│   ├── main.c
│   ├── can_socket.c
│   ├── cli_parser.c
│   ├── frame_logger.c
│   ├── ring_buffer.c
│   ├── signal_handler.c
│   └── statistics.c
├── tests/
│   ├── CMakeLists.txt
│   ├── unity/              # Unity-compatible test framework (MIT)
│   │   ├── unity.h
│   │   └── unity.c
│   ├── test_ring_buffer.c
│   ├── test_statistics.c
│   ├── test_cli_parser.c
│   └── test_frame_logger.c
├── docs/
│   └── canbus_monitor.1    # man page
└── scripts/
    ├── build.sh            # CMake build wrapper
    └── setup_vcan.sh       # Virtual CAN interface setup
```

---

## Contributing

Patches welcome. Please:

1. Follow the existing coding style (C11, Linux kernel naming conventions)
2. Add or update unit tests for any changed module
3. Ensure `ctest` passes before submitting
4. Include a `Signed-off-by` line (Developer Certificate of Origin)

---

## License

This program is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License** as published by the Free
Software Foundation, either **version 3** of the License, or (at your option)
any later version.

See [LICENSE](LICENSE) or <https://www.gnu.org/licenses/gpl-3.0.html>.

Copyright (C) 2026  Ahmad Rashed
