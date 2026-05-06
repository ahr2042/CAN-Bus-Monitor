# CAN Bus Monitor — Internal Architecture & Developer Reference

**Version:** 1.0.0  
**Language:** C11  
**Platform:** Linux (SocketCAN)  
**License:** GPL-3.0-or-later  
**Copyright:** 2026 Ahmad Rashed

---

## Table of Contents

1. [Project Overview](#1-project-overview)
2. [Repository Structure](#2-repository-structure)
3. [Architecture Overview](#3-architecture-overview)
   - 3.1 [Threading Model](#31-threading-model)
   - 3.2 [Data Flow](#32-data-flow)
   - 3.3 [Module Dependency Graph](#33-module-dependency-graph)
4. [Build System](#4-build-system)
   - 4.1 [CMake Configuration](#41-cmake-configuration)
   - 4.2 [Compiler Flags & Sanitizers](#42-compiler-flags--sanitizers)
   - 4.3 [Build Scripts](#43-build-scripts)
5. [SocketCAN Primer](#5-socketcan-primer)
6. [Module Reference](#6-module-reference)
   - 6.1 [main — Event Loop & Orchestration](#61-main--event-loop--orchestration)
   - 6.2 [can_socket — SocketCAN Abstraction](#62-can_socket--socketcan-abstraction)
   - 6.3 [ring_buffer — Lock-Free SPSC Queue](#63-ring_buffer--lock-free-spsc-queue)
   - 6.4 [frame_logger — ASC Log Writer](#64-frame_logger--asc-log-writer)
   - 6.5 [statistics — Per-ID Metrics](#65-statistics--per-id-metrics)
   - 6.6 [signal_handler — Self-Pipe Signal Handling](#66-signal_handler--self-pipe-signal-handling)
   - 6.7 [cli_parser — Argument Parsing](#67-cli_parser--argument-parsing)
7. [Key Algorithms & Data Structures](#7-key-algorithms--data-structures)
   - 7.1 [SPSC Ring Buffer Internals](#71-spsc-ring-buffer-internals)
   - 7.2 [FNV-1a Hash Table](#72-fnv-1a-hash-table)
   - 7.3 [Welford Online Mean & Variance](#73-welford-online-mean--variance)
   - 7.4 [Self-Pipe Trick](#74-self-pipe-trick)
8. [Concurrency Model](#8-concurrency-model)
9. [ASC Log Format](#9-asc-log-format)
10. [Error Handling Conventions](#10-error-handling-conventions)
11. [Configuration Reference](#11-configuration-reference)
12. [Test Suite](#12-test-suite)
13. [Scripts Reference](#13-scripts-reference)

---

## 1. Project Overview

`canbus-monitor` is a Linux userspace daemon that captures CAN frames from any
SocketCAN-compatible interface (hardware or virtual), writes them to rotating
ASC-format log files, and computes per-CAN-ID bus statistics.

**Design goals:**

| Goal | Implementation |
|---|---|
| Zero-copy receive path | `AF_CAN / SOCK_RAW` with kernel-level ID filtering |
| Non-blocking logging | Lock-free SPSC ring buffer decouples receive from disk I/O |
| Accurate timestamping | Hardware timestamps via `SO_TIMESTAMPING`; software fallback |
| Graceful shutdown | Self-pipe signal handler wakes `poll()` without busy-waiting |
| Log interoperability | ASC format compatible with CANalyzer, python-can, cantools |
| Portability within Linux | Pure C11 + POSIX + Linux-specific headers; no external libraries |

---

## 2. Repository Structure

```
canbus-monitor/
│
├── CMakeLists.txt          # Top-level CMake build definition
├── LICENSE                 # GNU GPL v3.0+
├── README.md               # Quick-start guide
│
├── include/                # Public API headers (one per module)
│   ├── can_socket.h        # SocketCAN socket lifecycle & receive
│   ├── cli_parser.h        # Argument parsing & runtime config struct
│   ├── frame_logger.h      # ASC logging, rotation, flush thread
│   ├── ring_buffer.h       # Lock-free SPSC ring buffer
│   ├── signal_handler.h    # SIGINT/SIGTERM/SIGHUP handling
│   └── statistics.h        # Per-ID & aggregate CAN statistics
│
├── src/                    # Implementation files
│   ├── main.c              # poll() event loop, startup/teardown
│   ├── can_socket.c        # Socket creation, filters, recv + timestamps
│   ├── cli_parser.c        # getopt_long argument parsing
│   ├── frame_logger.c      # File I/O, rotation, flush pthread
│   ├── ring_buffer.c       # C11 atomic SPSC ring buffer
│   ├── signal_handler.c    # sigaction + self-pipe implementation
│   └── statistics.c        # FNV-1a hash table + Welford statistics
│
├── tests/                  # Unit test suite (Unity framework)
│   ├── CMakeLists.txt      # Test build definitions
│   ├── unity/              # Vendored Unity framework (MIT licence)
│   │   ├── unity.h
│   │   └── unity.c
│   ├── test_ring_buffer.c  # 10 tests
│   ├── test_statistics.c   # 10 tests
│   ├── test_cli_parser.c   # 13 tests
│   └── test_frame_logger.c # 8 tests
│
├── docs/
│   ├── canbus_monitor.1    # man page
│   └── INTERNALS.md        # This document
│
└── scripts/
    ├── build.sh            # CMake wrapper (debug / release / clean)
    └── setup_vcan.sh       # Virtual CAN interface setup helper
```

Every public header uses a unique include guard of the form
`CANBUS_MONITOR_<MODULE>_H` to prevent double-inclusion.  Headers are
designed to be self-contained — each includes exactly the standard or
system headers it needs rather than relying on transitive inclusion.

---

## 3. Architecture Overview

### 3.1 Threading Model

The program runs with **two threads**:

| Thread | Role | Blocking calls |
|---|---|---|
| **Main thread** | `poll()` event loop; receives CAN frames; updates statistics | `poll(2)` |
| **Flush thread** | Drains the ring buffer to disk; calls `fwrite`/`fflush` | `nanosleep(2)`, `fwrite(3)` |

The flush thread is started inside `frame_logger_create()` and joined inside
`frame_logger_destroy()`.  All file I/O is isolated to the flush thread,
which means the main receive path **never blocks on disk**.

Shared state between the two threads:

| Resource | Protection mechanism |
|---|---|
| Ring buffer (`ring_buffer_t`) | C11 `_Atomic` head/tail indices (lock-free SPSC) |
| Log file pointer (`FILE *fp`) | `pthread_mutex_t file_mutex` inside `frame_logger_t` |
| Stop flag (`flush_stop`) | `_Atomic(bool)` |

The statistics structure is accessed **only from the main thread** (no
concurrent writes), so its internal hash table does not need a lock.  The
aggregate counters (`total_frames`, `total_bytes`, `dropped_frames`,
`log_errors`) are `_Atomic(uint64_t)` to allow safe atomic increments from
the main thread while a future reader (e.g. a monitoring thread) could read
them without races.

### 3.2 Data Flow

```
┌────────────────────────────────────────────────────────────────┐
│                        MAIN THREAD                             │
│                                                                │
│   poll(can_fd, sig_fd)                                         │
│        │                                                       │
│        ├─── POLLIN on can_fd ──► can_socket_recv()            │
│        │                              │                        │
│        │                              ├─► frame_logger_write() │
│        │                              │     └─► rb_push()      │
│        │                              │         (lock-free)    │
│        │                              │                        │
│        │                              ├─► stats_update()       │
│        │                              │   (hash table, no lock)│
│        │                              │                        │
│        │                              └─► print_frame()        │
│        │                                  (if --verbose)       │
│        │                                                       │
│        └─── POLLIN on sig_fd ──► drain pipe byte              │
│                                   set exit/rotate flag         │
└───────────────────────────┬────────────────────────────────────┘
                            │  ring_buffer_t  (SPSC, C11 atomics)
┌───────────────────────────▼────────────────────────────────────┐
│                       FLUSH THREAD                             │
│                                                                │
│   loop until flush_stop:                                       │
│     rb_pop() ──► fwrite() ASC line ──► fflush()               │
│     rotate file if current_bytes >= max_file_bytes             │
│     nanosleep(flush_interval_ms)                               │
└────────────────────────────────────────────────────────────────┘
```

### 3.3 Module Dependency Graph

```
main.c
  ├── can_socket.h      (open socket, recv frames, set filters)
  ├── frame_logger.h    (write frames to ASC log)
  │     └── ring_buffer.h  (internal: enqueue log entries)
  ├── statistics.h      (update per-ID metrics)
  ├── signal_handler.h  (check exit/rotate flags, get wake fd)
  └── cli_parser.h      (parse argv into cli_config_t)
        ├── frame_logger.h   (embeds frame_logger_cfg_t)
        └── can_socket.h     (embeds can_filter_spec_t)
```

`ring_buffer` has no dependencies on other project modules — it is a
self-contained generic data structure.  `statistics` and `can_socket` depend
only on `<linux/can.h>` for the `can_frame` and `canid_t` types.

---

## 4. Build System

### 4.1 CMake Configuration

**Minimum CMake version:** 3.18  
**Build directory:** `build/` (created by `build.sh` or manually)

Key CMake settings (`CMakeLists.txt`):

```cmake
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)           # -std=c11, not -std=gnu11
add_compile_definitions(_GNU_SOURCE)  # Expose full POSIX + GNU extensions
```

`_GNU_SOURCE` is required because the project uses:
- `sigaction`, `SA_RESTART`, `sigemptyset` (POSIX signal extensions)
- `clock_gettime`, `CLOCK_REALTIME` (POSIX timers)
- `pipe`, `O_NONBLOCK`, `FD_CLOEXEC` (POSIX file control)
- `localtime_r`, `asctime_r` (POSIX reentrant time functions)
- `getopt_long` (GNU extension for long CLI options)
- `mkdtemp` (POSIX.1-2008, used in tests)

Setting `CMAKE_C_EXTENSIONS OFF` tells GCC to use `-std=c11` (strict ISO C11)
rather than `-std=gnu11` (GNU dialect).  `_GNU_SOURCE` then selectively
re-enables the needed system-level extensions without enabling every GNU
C dialect extension at the language level.

**Targets:**

| CMake target | Output | Description |
|---|---|---|
| `canbus_monitor` | `build/canbus_monitor` | Main executable |
| `test_ring_buffer` | `build/tests/test_ring_buffer` | Ring buffer unit tests |
| `test_statistics` | `build/tests/test_statistics` | Statistics unit tests |
| `test_cli_parser` | `build/tests/test_cli_parser` | CLI parser unit tests |
| `test_frame_logger` | `build/tests/test_frame_logger` | Frame logger unit tests |

**Linked libraries:**

| Library | Targets | Purpose |
|---|---|---|
| `Threads::Threads` (pthreads) | `canbus_monitor` | Flush thread in `frame_logger` |
| `rt` | `canbus_monitor` | `clock_gettime` on older glibc |
| `m` | `canbus_monitor`, `test_statistics` | `sqrt()` in `stats_print_report` |

### 4.2 Compiler Flags & Sanitizers

All targets receive the following warning flags:

| Flag | Purpose |
|---|---|
| `-Wall` | Common warnings (unused vars, missing return, etc.) |
| `-Wextra` | Additional checks (signed/unsigned comparison, etc.) |
| `-Wpedantic` | ISO C11 conformance warnings |
| `-Wshadow` | Warn when a local variable shadows an outer scope variable |
| `-Wformat=2` | Strict printf/scanf format string checking |
| `-Wconversion` | Implicit type conversions that may lose data |
| `-Werror=implicit-function-declaration` | Undeclared function calls are hard errors |

**Debug build** additionally enables:
- `-g3` — Full debug info including macros
- `-fsanitize=address,undefined` — AddressSanitizer (heap/stack overflows, use-after-free) and UndefinedBehaviorSanitizer (integer overflow, misaligned access, etc.)

**Release build** additionally enables:
- `-O2` — Optimisation level 2
- `-DNDEBUG` — Disables `assert()` macros

> **Note on generator expressions:** Each flag is given its own
> `$<$<CONFIG:Debug>:FLAG>` expression.  Combining multiple flags into one
> expression (e.g. `$<$<CONFIG:Debug>:-g3 -fsanitize=address,undefined>`)
> causes CMake to pass the entire space-separated string as a single compiler
> argument, resulting in `undefined>` being misinterpreted as an fsanitize
> value.

### 4.3 Build Scripts

**`scripts/build.sh [debug|release] [clean]`**

```
Usage:
  ./scripts/build.sh           # Debug build (default)
  ./scripts/build.sh release   # Optimised release build
  ./scripts/build.sh debug clean  # Delete build/ then rebuild
```

Internally the script:
1. Normalises the build type string (case-insensitive)
2. Optionally runs `rm -rf build/`
3. Runs `cmake -S . -B build -DCMAKE_BUILD_TYPE=<type> -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
4. Runs `cmake --build build --parallel $(nproc)` to use all CPU cores

`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` produces `build/compile_commands.json`,
which enables accurate code navigation in editors such as VSCode (clangd) or
CLion.

---

## 5. SocketCAN Primer

SocketCAN is the Linux kernel's standard CAN bus subsystem.  It exposes CAN
interfaces as regular network interfaces (`ip link show type can`) and CAN
frames as BSD-style sockets.

**Socket creation:**

```c
int fd = socket(AF_CAN, SOCK_RAW, CAN_RAW);
```

`AF_CAN` is the CAN address family.  `SOCK_RAW` means we receive every frame
on the bus (subject to kernel filters).  `CAN_RAW` is the only supported
protocol for user-space raw access.

**Frame structure** (`<linux/can.h>`):

```c
struct can_frame {
    canid_t can_id;      /* 32-bit: upper bits = flags, lower bits = ID */
    uint8_t can_dlc;     /* Data Length Code: 0–8 bytes                 */
    uint8_t pad, res0, res1;
    uint8_t data[CAN_MAX_DLEN];  /* CAN_MAX_DLEN = 8                    */
};
```

`can_id` bit layout:

| Bits | Meaning |
|---|---|
| 28:0 | CAN ID (11-bit: bits 10:0 only; 29-bit: bits 28:0) |
| 29 | `CAN_ERR_FLAG` — error frame |
| 30 | `CAN_RTR_FLAG` — Remote Transmission Request |
| 31 | `CAN_EFF_FLAG` — Extended Frame Format (29-bit ID) |

**Kernel-level filtering** (`CAN_RAW_FILTER`):

Filters are applied in the kernel before the frame reaches userspace.  Each
filter has an `id` and a `mask`:

```
frame passes if (frame.can_id & mask) == (id & mask)
```

This eliminates the overhead of copying unwanted frames to userspace.  Up to
`SOL_CAN_RAW` filter entries can be installed at once (practical limit is
~32 for most drivers).

**Timestamps** (`SO_TIMESTAMPING`):

The monitor requests four timestamping flags:
- `SOF_TIMESTAMPING_RX_HARDWARE` — use NIC hardware clock if available
- `SOF_TIMESTAMPING_RX_SOFTWARE` — fall back to kernel software clock
- `SOF_TIMESTAMPING_SOFTWARE` — report the software stamp
- `SOF_TIMESTAMPING_RAW_HARDWARE` — report the raw hardware stamp

Timestamps are retrieved via `recvmsg()` ancillary data.  The kernel fills
a `scm_timestamping` structure with three `struct timespec` slots:
- `ts[0]` — software timestamp
- `ts[1]` — deprecated (always zero)
- `ts[2]` — hardware timestamp (raw)

Hardware (`ts[2]`) is preferred when non-zero; otherwise software (`ts[0]`)
is used.  If neither ancillary timestamp is available (e.g. vcan), the code
falls back to `clock_gettime(CLOCK_REALTIME)`.

---

## 6. Module Reference

### 6.1 `main` — Event Loop & Orchestration

**File:** `src/main.c`

`main()` performs the following steps in order:

#### Startup sequence

1. **`cli_parse()`** — Parse `argv` into `cli_config_t`.  On `--help` or
   `--version`, the function returns `-1` and main exits cleanly with
   `EXIT_SUCCESS`.
2. **`sh_install()`** — Install signal handlers and create the self-pipe.
   Must be called before spawning threads so signals go to the main thread.
3. **`can_socket_open()`** — Open and bind the raw SocketCAN socket.
4. **`can_socket_set_filters()`** — Install kernel ID filters (if specified).
5. **`frame_logger_create()`** — Allocate logger, open first log file, start
   flush thread.
6. **`stats_create(0)`** — Allocate the statistics hash table with the
   default initial capacity (256 buckets).

#### Poll loop

```c
struct pollfd pfds[2] = {
    { .fd = can_socket_get_fd(sock), .events = POLLIN },
    { .fd = sh_get_wake_fd(),        .events = POLLIN },
};
```

`poll()` blocks on both the CAN socket and the signal self-pipe.  When it
returns:

- **`pfds[1]` (signal pipe) readable:** Drain the pipe (the byte content is
  not used; its presence is the notification).  The loop continues to the
  top where `sh_termination_requested()` may cause an exit.
- **`pfds[0]` (CAN socket) readable:** Call `can_socket_recv()`, then
  `stats_update()`, `frame_logger_write()`, and optionally `print_frame()`.
- **`errno == EINTR`:** A signal interrupted `poll()` before either fd was
  ready.  `continue` to the top; the atomic flag will be checked.

**Frame count limit:** If `cfg.max_frames > 0`, the loop exits cleanly once
`frame_count >= cfg.max_frames`.

**SIGHUP handling:** Checked at the top of each loop iteration via
`sh_rotation_requested()`.  When set, the logger is flushed to close the
current segment.  A new segment is opened automatically the next time data
is written.

#### Teardown sequence

1. Final `frame_logger_flush()` — Drain any remaining ring buffer entries.
2. `frame_logger_destroy()` — Signal flush thread to stop, join it, close
   file, free memory.
3. `stats_print_report()` — Printed only if `--stats` was specified.
4. `stats_destroy()` — Free hash table.
5. `can_socket_close()` — Close the socket fd and free handle.

#### Helper: `format_can_id()`

Formats a `canid_t` for human-readable stdout output:
- Standard 11-bit IDs: `%03X` (e.g. `1A0`)
- Extended 29-bit IDs: `%08X X` (e.g. `0CF00400 X`)
- Error frames: `ERR %08X`

#### Helper: `print_frame()`

Formats and prints a single CAN frame to stdout in a style similar to
`candump`:

```
(  1.234567)  vcan0     1A0           [4]  01 02 03 04
```

Fields: `(timestamp_s)`, interface name, CAN ID, `[dlc]`, hex data bytes.

---

### 6.2 `can_socket` — SocketCAN Abstraction

**Files:** `src/can_socket.c`, `include/can_socket.h`

Provides a thin, error-checked wrapper around the raw SocketCAN API.  All
public functions return negative `errno` values on failure.

#### Internal structure

```c
struct can_socket {
    int          fd;          /* Raw socket file descriptor */
    unsigned int timeout_ms;  /* Configured receive timeout */
};
```

The struct is **opaque** — callers only see `can_socket_t *`.  This hides
the raw fd and prevents callers from bypassing the API.

#### `can_socket_open(iface, timeout_ms)`

1. Validates `iface` is non-NULL and non-empty.
2. `calloc` the handle.
3. `socket(AF_CAN, SOCK_RAW, CAN_RAW)` — opens the socket.
4. `ioctl(SIOCGIFINDEX)` via `struct ifreq` — resolves the interface name
   to a kernel interface index.
5. `bind(struct sockaddr_can)` — binds the socket to the specific interface.
6. `setsockopt(CAN_RAW_ERR_FILTER, CAN_ERR_MASK)` — enables error frames so
   bus-off and error-passive conditions are visible.
7. `enable_hardware_timestamps()` — calls `setsockopt(SO_TIMESTAMPING)`.
   Non-fatal: the socket works without hardware timestamps.
8. `set_receive_timeout()` — configures `SO_RCVTIMEO` if `timeout_ms > 0`.
   Non-fatal: the socket remains usable in blocking mode if this fails.

Error resources are cleaned up at each failure point via the saved-errno
pattern:

```c
int saved = errno;
close(cs->fd);
free(cs);
errno = saved;
return NULL;
```

#### `can_socket_set_filters(cs, filters, count)`

Translates `can_filter_spec_t[]` (project abstraction) into
`struct can_filter[]` (kernel type from `<linux/can.h>`) and installs them
via `setsockopt(SOL_CAN_RAW, CAN_RAW_FILTER)`.

If `count == 0`, passes `NULL` and length 0 to the socket option, which
instructs the kernel to drop all frames (useful for safety shutdown).

The temporary `kfilters` array is always freed before returning.

#### `can_socket_recv(cs, frame, ts_ns)`

Uses `recvmsg()` rather than `read()` to receive the frame and its ancillary
timestamp data in a single syscall.

The ancillary buffer is sized with `CMSG_SPACE(sizeof(struct scm_timestamping))`
which adds the necessary alignment padding.

The timestamp extraction loop:
```c
for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
```
Walks the control message chain looking for `SOL_SOCKET / SO_TIMESTAMPING`.
When found, it extracts `scm_timestamping.ts[2]` (hardware) if non-zero,
falling back to `ts[0]` (software).

Returns:
- `1` — frame received
- `0` — timeout (EAGAIN/EWOULDBLOCK)
- `-errno` — error

---

### 6.3 `ring_buffer` — Lock-Free SPSC Queue

**Files:** `src/ring_buffer.c`, `include/ring_buffer.h`

A generic, type-erased, single-producer / single-consumer ring buffer using
C11 `_Atomic` indices.  Items are copied by value (`memcpy`); the buffer
owns no item pointers.

#### Internal structure

```c
struct ring_buffer {
    _Atomic(size_t) head;      /* Next write slot (owned by producer) */
    _Atomic(size_t) tail;      /* Next read slot  (owned by consumer) */
    size_t          mask;      /* capacity - 1 (for power-of-two wrap) */
    size_t          item_size;
    uint8_t        *data;      /* Heap-allocated flat item array */
};
```

`head` and `tail` are never reset; they grow monotonically and wrap via
bitwise AND with `mask`.  This simplifies the full/empty check:

```c
/* Full  */ next_head == tail   (one slot is always kept empty — the sentinel)
/* Empty */ head      == tail
```

See [Section 7.1](#71-spsc-ring-buffer-internals) for the full atomics
analysis.

#### `rb_create(capacity, item_size)`

The requested `capacity` is rounded up to the next power of two by
`next_pow2()`.  One extra slot is added before rounding to accommodate the
sentinel:

```c
size_t real_cap = next_pow2(capacity + 1u);
```

So `rb_create(4, ...)` allocates 8 slots (next power of two after 4+1=5) and
exposes 7 usable slots via `rb_capacity()`.

`next_pow2()` uses bit-smearing:
```c
--n;
n |= n >> 1; n |= n >> 2; n |= n >> 4;
n |= n >> 8; n |= n >> 16;
#if SIZE_MAX > 0xFFFFFFFF
n |= n >> 32;  /* 64-bit platforms */
#endif
return n + 1;
```

#### `rb_push(rb, item)` — Producer side

```c
size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
size_t next = (head + 1u) & rb->mask;
if (next == atomic_load_explicit(&rb->tail, memory_order_acquire))
    return false;  /* full */
memcpy(rb->data + head * rb->item_size, item, rb->item_size);
atomic_store_explicit(&rb->head, next, memory_order_release);
return true;
```

- `relaxed` load of `head` — only the producer writes `head`, so no
  synchronisation is needed to read it.
- `acquire` load of `tail` — synchronises with the consumer's `release`
  store of `tail` to ensure the slot the producer is about to write into
  has been fully consumed.
- `release` store of `head` — makes the written item visible to the consumer
  before the index advance is seen.

#### `rb_pop(rb, out)` — Consumer side

```c
size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
if (tail == atomic_load_explicit(&rb->head, memory_order_acquire))
    return false;  /* empty */
memcpy(out, rb->data + tail * rb->item_size, rb->item_size);
atomic_store_explicit(&rb->tail, (tail + 1u) & rb->mask, memory_order_release);
return true;
```

Symmetric to `rb_push`: `acquire` load of `head` synchronises with the
producer's `release` store; `release` store of `tail` makes the slot
available to the producer.

---

### 6.4 `frame_logger` — ASC Log Writer

**Files:** `src/frame_logger.c`, `include/frame_logger.h`

Manages the full lifecycle of ASC log files: creation, writing, rotation,
background flushing, and cleanup.

#### Internal structure

```c
struct frame_logger {
    frame_logger_cfg_t cfg;           /* Copy of user config          */
    ring_buffer_t     *ring;          /* SPSC queue of log_entry_t    */
    FILE              *fp;            /* Current open log file        */
    size_t             current_bytes; /* Bytes written to current file*/
    unsigned int       seq;           /* File sequence number         */
    char             **history;       /* Circular array of file paths */
    unsigned int       history_head;  /* Index into history[]         */
    pthread_t          flush_thread;  /* Background flush thread      */
    _Atomic(bool)      flush_stop;    /* Set to stop the flush thread */
    pthread_mutex_t    file_mutex;    /* Protects fp + current_bytes  */
};
```

The `history` array is a circular buffer of `max_rotations` string pointers,
each holding a `strdup`-ed path of a past log file.  When a new file opens,
the oldest entry is `unlink`-ed before being replaced.

#### Log entry format

Each CAN frame is formatted into a `log_entry_t`:

```c
typedef struct {
    char   data[128];   /* Formatted ASC line */
    size_t len;         /* Length of valid data */
} log_entry_t;
```

The format string used in `frame_logger_write()`:

```c
"%12.6f  %u  %0*" PRIX32 "  Rx  d  %u  %s\n"
```

Fields: `timestamp_s`, `channel`, `id_width` (3 for SFF, 8 for EFF),
`can_id`, `dlc`, `data_hex`.

The `PRIX32` macro (from `<inttypes.h>`) ensures correct portable formatting
of the 32-bit CAN ID as uppercase hex.

#### File naming

Log files are named using the pattern:

```
<base_path>/<prefix>_YYYYMMDD_HHMMSS_<seq>.asc
```

Example: `canbus_logs/can0_20260428_170001_0000.asc`

The timestamp comes from `localtime_r()` (reentrant) and the sequence counter
increments with each rotation.

#### ASC file header

Written once per file at creation:

```
date Tue Apr 28 17:00:01 2026
base hex  timestamps absolute
no internal events logged
// canbus_monitor v1.0.0
```

#### Flush thread

`flush_thread_fn()` loops:
1. Call `flush_ring()` — drain all pending `log_entry_t` from the ring
   buffer to disk under `file_mutex`.
2. After each drain, check `current_bytes >= max_file_bytes` and rotate if
   needed.
3. `nanosleep(flush_interval_ms)` — sleep between flushes.
4. Exit when `flush_stop` is set (set by `frame_logger_destroy()`).

The flush thread performs one final `flush_ring()` call after `flush_stop`
is set to ensure no frames are lost during shutdown.

#### Rotation algorithm

`open_new_log_file()` is called both at startup and during rotation:
1. `build_log_path()` — compute the next filename.
2. `fopen(path, "w")` — open the new file.
3. Write the ASC header.
4. Update `history[]`: `strdup(path)` into `history[history_head]`.
5. Compute `oldest = (history_head + 1) % max_rotations` and `unlink()` it.
6. Advance `history_head`.
7. Increment `seq`.

This ensures at most `max_rotations` files exist at any time.

#### Thread safety summary

`frame_logger_write()` is called from the **main thread** and only calls
`rb_push()` — no mutex required.  The ring buffer's lock-free design is safe
for one producer (main) and one consumer (flush thread).

All file operations (`fopen`, `fwrite`, `fclose`, `fflush`) are performed
under `file_mutex` in the flush thread.  The only cross-thread operation on
the file handle is via `rb_pop()` in the flush thread.

---

### 6.5 `statistics` — Per-ID Metrics

**Files:** `src/statistics.c`, `include/statistics.h`

Maintains per-CAN-ID statistics in a hash table with open addressing.
Aggregate counters (total frames, bytes, dropped, errors) use C11 atomics.

#### Internal structure

```c
struct statistics {
    bucket_t          *buckets;
    size_t             capacity;    /* Current hash table capacity (power of 2) */
    size_t             size;        /* Number of occupied buckets               */
    _Atomic(uint64_t)  total_frames;
    _Atomic(uint64_t)  total_bytes;
    _Atomic(uint64_t)  dropped_frames;
    _Atomic(uint64_t)  log_errors;
};

typedef struct {
    bool          occupied;
    stats_entry_t entry;    /* Public per-ID data (see statistics.h)       */
    double        dlc_m2;   /* Welford second moment for DLC variance       */
    double        interval_m2;  /* Welford second moment for interval variance */
    uint64_t      welford_n;    /* Sample count for Welford algorithm         */
} bucket_t;
```

#### Per-ID record (`stats_entry_t`)

```c
typedef struct {
    canid_t  id;
    uint64_t frame_count;
    uint64_t byte_count;
    uint8_t  dlc_min, dlc_max;
    double   dlc_mean;
    uint64_t ts_first_ns, ts_last_ns;
    double   interval_mean_us;
    double   interval_var_us;    /* Welford online variance */
} stats_entry_t;
```

#### `stats_update()` — Hot path

Called on every non-error frame from the main receive loop:

1. **Resize check:** If load factor `size/capacity >= 0.7`, call
   `ht_resize(capacity * 2)` before inserting.
2. **ID masking:** `id = frame->can_id & CAN_EFF_MASK` — strip flag bits.
3. **Hash lookup:** `ht_find_or_insert(s, id)` — linear probing starting at
   `fnv1a_u32(id) & (capacity - 1)`.
4. **Atomic aggregate counters:** `atomic_fetch_add` on `total_frames` and
   `total_bytes`.
5. **DLC tracking:** Direct min/max comparison; Welford mean update.
6. **Interval timing:** On `frame_count > 1`, compute `interval_us` from
   `ts_ns - entry->ts_last_ns` and run the Welford algorithm on the interval.

See [Section 7.3](#73-welford-online-mean--variance) for the Welford
algorithm details.

#### `stats_print_report()`

Copies all occupied bucket entries into a heap-allocated `stats_entry_t[]`,
sorts descending by `frame_count` using `qsort()`, then prints a formatted
table with columns:

```
CAN ID    Frames     Bytes  dMin  dMax  dMean    us Mean   us StdDev
```

`sqrt(interval_var_us)` converts the Welford variance to standard deviation
for the final column.

---

### 6.6 `signal_handler` — Self-Pipe Signal Handling

**Files:** `src/signal_handler.c`, `include/signal_handler.h`

Handles `SIGINT`, `SIGTERM`, and `SIGHUP` using a combination of:
- `sigaction(2)` for reliable signal installation
- A **self-pipe** so `poll()` wakes immediately on signal receipt

#### Module-private state

```c
static _Atomic(bool) s_terminate = false;   /* SIGINT / SIGTERM */
static _Atomic(bool) s_rotate    = false;   /* SIGHUP           */
static int           s_pipe_rd   = -1;      /* poll() reads this */
static int           s_pipe_wr   = -1;      /* handler writes this */
```

#### `sh_install()`

1. **Create the pipe:** `pipe(pipefd)` — two file descriptors.
2. **Set both ends non-blocking:** `fcntl(F_SETFL, O_NONBLOCK)` — prevents
   the signal handler from blocking if the pipe's 64 KiB kernel buffer fills
   up (would deadlock the process).
3. **Set close-on-exec:** `fcntl(F_SETFD, FD_CLOEXEC)` — child processes
   spawned via `fork/exec` do not inherit the pipe.
4. **Install `sigaction`** for `SIGINT`, `SIGTERM`, `SIGHUP`:
   - `sa_handler = signal_handler_fn`
   - `sa_flags = SA_RESTART` — system calls interrupted by these signals
     are automatically restarted (avoids spurious `EINTR` from `read()`/
     `write()` on the CAN socket).
   - `sigemptyset(&sa_mask)` — do not block any additional signals during
     the handler.

#### `signal_handler_fn()` — async-signal-safe

Only async-signal-safe operations are performed inside the handler:
- `atomic_store` (C11 atomics are signal-safe)
- `write(2)` on the self-pipe write end

The byte value written to the pipe is the signal number cast to `uint8_t`,
though the receiver discards it (any byte wakes the poll).

```c
static void signal_handler_fn(int signum)
{
    const uint8_t byte = (uint8_t)signum;
    switch (signum) {
    case SIGINT: case SIGTERM: atomic_store(&s_terminate, true); break;
    case SIGHUP:               atomic_store(&s_rotate, true);    break;
    }
    if (s_pipe_wr >= 0)
        (void)write(s_pipe_wr, &byte, 1u);
}
```

#### `sh_rotation_requested()` — edge-triggered

Uses `atomic_exchange` to **atomically read and clear** the flag:

```c
return atomic_exchange(&s_rotate, false);
```

This gives edge-triggered semantics: a second call in the same loop
iteration will return `false` even if `SIGHUP` was received between the
two calls.

---

### 6.7 `cli_parser` — Argument Parsing

**Files:** `src/cli_parser.c`, `include/cli_parser.h`

Parses `argc/argv` into a `cli_config_t` struct using `getopt_long(3)`.

#### `cli_config_t` — Runtime configuration

```c
typedef struct {
    const char         *iface;        /* e.g. "can0" (points into argv) */
    unsigned int        timeout_ms;   /* Socket receive timeout          */
    frame_logger_cfg_t  logger;       /* Embedded logger config          */
    bool                log_enabled;  /* Always true in current version  */
    can_filter_spec_t   filters[32];  /* Up to 32 kernel ID filters      */
    size_t              filter_count;
    bool                show_stats;
    bool                verbose;
    uint64_t            max_frames;   /* 0 = unlimited                   */
} cli_config_t;
```

All string pointers (`iface`, `logger.base_path`, `logger.prefix`) point
directly into `argv[]` — they must not be freed and remain valid for the
process lifetime.

#### Default values

| Field | Default |
|---|---|
| `timeout_ms` | 200 ms |
| `logger.base_path` | `"./canbus_logs"` |
| `logger.prefix` | Interface name (e.g. `"can0"`) |
| `logger.max_file_bytes` | 50 MiB (52 428 800 bytes) |
| `logger.flush_interval_ms` | 500 ms |
| `logger.max_rotations` | 10 |
| `max_frames` | 0 (unlimited) |

#### Filter parsing

`--filter id:mask` where both values are hexadecimal.  The colon separator
is optional; if omitted, `mask` defaults to `0x7FF` (standard 11-bit exact
match).

```c
out->id   = strtoul(str, &end, 16);
out->mask = colon ? strtoul(colon + 1, &end, 16) : 0x7FFu;
```

#### `optind` reset

`cli_parse()` explicitly resets `optind = 1` before calling `getopt_long`.
This allows unit tests to call `cli_parse()` multiple times in the same
process without state leaking between calls.

---

## 7. Key Algorithms & Data Structures

### 7.1 SPSC Ring Buffer Internals

The ring buffer implements a classic **single-producer, single-consumer
(SPSC)** queue.  Its safety guarantee relies on two properties:

1. **Ownership:** `head` is written **only** by the producer; `tail` is
   written **only** by the consumer.  Each thread reads the other's index
   to detect full/empty, but never writes it.

2. **Memory ordering:** C11 `memory_order_acquire` / `memory_order_release`
   pairs ensure that:
   - The consumer sees the item data **after** the producer's `release` store
     of the new `head`.
   - The producer sees the freed slot **after** the consumer's `release` store
     of the new `tail`.

**Why power of two?**  Index wrapping uses `& mask` instead of `% capacity`:

```c
next = (head + 1u) & rb->mask;   /* mask = capacity - 1 */
```

Modulo by a power of two is equivalent to AND with `(pow2 - 1)` and executes
in a single CPU cycle rather than a division.

**Sentinel slot:**  The buffer always keeps one slot empty.  Without this,
`head == tail` would be ambiguous (could mean empty or full).  The sentinel
slot makes the invariants unambiguous:
- `head == tail` → empty
- `(head + 1) & mask == tail` → full

**No ABA problem:**  Unlike compare-and-swap based queues, SPSC with single
ownership of each index is inherently ABA-free.

### 7.2 FNV-1a Hash Table

The statistics module uses a **hash table with open addressing and linear
probing**.

**Hash function** (FNV-1a, 32-bit):

```c
static uint32_t fnv1a_u32(uint32_t val)
{
    uint32_t h = 2166136261u;    /* FNV offset basis */
    for (int i = 0; i < 4; ++i) {
        h ^= (uint8_t)(val & 0xFF);
        h *= 16777619u;           /* FNV prime */
        val >>= 8;
    }
    return h;
}
```

FNV-1a processes each byte of the input, XORing it into the hash before
multiplying.  For the 32-bit CAN ID input, this gives excellent avalanche
(bit changes in any byte of the ID affect the full hash output) with very
low collision rate for the ID ranges used in CAN buses (0x000–0x7FF for SFF,
0x00000000–0x1FFFFFFF for EFF).

**Slot lookup** (`ht_find_or_insert`):

```c
size_t index = fnv1a_u32(id) & mask;   /* Initial slot */
for (size_t probe = 0; probe < capacity; probe++) {
    bucket_t *b = &buckets[(index + probe) & mask];
    if (!b->occupied) { /* insert new */ }
    if (b->entry.id == id) return b;  /* found */
}
return NULL;  /* table full (should never happen after resize) */
```

Linear probing is simple and cache-friendly.  Clustering can degrade worst-
case performance, but at 70% load factor (the resize trigger) the expected
probe length is `~1.67` slots.

**Dynamic resizing** (`ht_resize`):

Triggered when `size / capacity >= 0.7` (70% load factor):
1. Allocate a new `calloc`-zeroed bucket array at double capacity.
2. Re-hash all existing occupied entries into the new array by calling
   `ht_find_or_insert` (which finds the correct slot in the new layout).
3. Free the old array.

The 70% threshold balances memory usage against probe-chain length.  At
70% load with linear probing, successful lookups average ~1.7 comparisons
and unsuccessful lookups average ~3.2 comparisons.

### 7.3 Welford Online Mean & Variance

Both `dlc_mean` and `interval_mean_us` are computed using **Welford's online
algorithm**, which computes the running mean and variance in a single pass
without accumulating a large sum that could overflow or lose precision.

**Algorithm** for a new sample `x` with count `n`:

```c
delta  = x - mean
mean  += delta / n
m2    += delta * (x - mean)    /* m2 = sum of squared deviations */
variance = m2 / (n - 1)        /* unbiased (Bessel's correction) */
```

This is numerically stable for large `n` because each update only involves
the difference between the new sample and the current mean, not the raw
sum.  Summing `n` floating-point values naively has error of order
`O(n * eps)` where `eps` is machine epsilon; Welford's algorithm has error
of order `O(eps)` regardless of `n`.

For **DLC variance**, `n` is the frame count for that ID and `x` is
`frame->can_dlc`.  For **interval variance**, `n = frame_count - 1` (the
first frame has no preceding frame to compute an interval from) and
`x = interval_us`.

### 7.4 Self-Pipe Trick

The **self-pipe trick** is a classic POSIX technique to make signal delivery
compatible with `poll()`/`select()` event loops.

**Problem:** `poll()` blocks in the kernel.  If a signal arrives while
`poll()` is sleeping, the kernel wakes it with `EINTR`.  The signal handler
has run, but the main loop must now call `poll()` again — introducing one
full `poll()` timeout worth of latency.  With `SA_RESTART`, `poll()` is
automatically restarted and the signal is processed only at the top of the
next iteration.

**Solution:**
1. Create a `pipe(2)` at startup.  The read end is added to `poll`'s watch list.
2. The signal handler writes one byte to the write end.
3. `poll()` wakes immediately when the pipe becomes readable — in the same
   iteration the signal was delivered.
4. The main loop drains the pipe byte and checks the atomic flag.

This gives sub-millisecond signal response latency regardless of `poll()`
timeout settings.  The pipe is set `O_NONBLOCK` so a flood of signals
(write-end buffer full) never blocks the signal handler, which must
remain async-signal-safe.

---

## 8. Concurrency Model

```
Thread        | Reads                | Writes
─────────────────────────────────────────────────────────────────
Main          | ring_buffer.tail     | ring_buffer.head
              | (acquire)            | item payload, (release)
              |                      |
              | statistics.buckets   | statistics.buckets
              | (no sync needed;     | (sole writer)
              |  sole writer)        |
              |                      |
              | s_terminate          | (never)
              | s_rotate             | (never)
              | (atomic_load)        |
─────────────────────────────────────────────────────────────────
Flush thread  | ring_buffer.head     | ring_buffer.tail
              | (acquire)            | (release)
              |                      |
              | frame_logger.fp      | frame_logger.fp
              | (file_mutex)         | (file_mutex)
              |                      |
              | flush_stop           | (never)
              | (atomic_load)        |
─────────────────────────────────────────────────────────────────
Signal handler| (none)               | s_terminate (atomic_store)
(async)       |                      | s_rotate    (atomic_store)
              |                      | pipe write end (write(2))
```

**Key invariants:**

- The ring buffer is the **only** data path between the main thread and the
  flush thread.  No other data structure is shared between them without a
  lock.
- The `statistics` structure is accessed **exclusively** from the main
  thread.  The `_Atomic` aggregate counters allow safe future extension (e.g.
  a monitoring thread reading counters), but no such thread exists today.
- Signal handlers only write atomics and call `write(2)`, both of which are
  async-signal-safe per POSIX.
- `sh_install()` is called **before** any thread is spawned, so signal
  disposition is established before any thread could receive a signal.

---

## 9. ASC Log Format

ASC (ASCII Log Format) is a text-based CAN trace format originated by
Vector Informatik and widely supported by CAN analysis tools.

**File header** (written once per file):

```
date Tue Apr 28 17:00:01 2026
base hex  timestamps absolute
no internal events logged
// canbus_monitor v1.0.0
```

**Frame lines:**

```
    0.001234  1  1A0  Rx  d  4  01 02 03 04
    0.002501  1  0CF  Rx  d  8  FF FF 00 00 01 02 03 04
```

Field layout:

| Field | Example | Notes |
|---|---|---|
| Timestamp | `0.001234` | Seconds from epoch, 6 decimal places |
| Channel | `1` | Always 1 in this implementation |
| CAN ID | `1A0` | 3 hex digits for SFF; 8 digits for EFF |
| Direction | `Rx` | Always `Rx` (received frame) |
| Frame type | `d` | `d` = data frame |
| DLC | `4` | Number of data bytes |
| Data bytes | `01 02 03 04` | Space-separated uppercase hex |

**Extended frame IDs** use 8 hex digits without an `X` suffix in the frame
line (the `X` suffix is only used in the verbose stdout output via
`format_can_id()`).

**Compatibility:** Files produced by this tool can be read by:
- `python-can` — `can.ASCReader(filename)`
- `cantools` — `db.decode_message()`
- Vector CANalyzer / CANdb++ — File → Open

---

## 10. Error Handling Conventions

The project uses a consistent **negative errno** convention for all public
API functions:

| Return type | Success | Failure |
|---|---|---|
| `T *` (pointer) | Non-NULL pointer | `NULL` (errno set) |
| `int` | `0` or positive | `-errno` |
| `bool` | `true` | `false` (no errno) |
| `void` | — | — (NULL-safe) |

**errno preservation:** Whenever a function performs multiple syscalls and
needs to return an error from an early one, it saves errno before cleanup:

```c
int saved = errno;
close(fd);
free(ptr);
errno = saved;
return NULL;
```

This ensures the caller sees the original error code, not an error from the
cleanup operations.

**NULL safety:** All `*_destroy()` functions are explicitly documented and
implemented to accept NULL without crashing:

```c
void rb_destroy(ring_buffer_t *rb)
{
    if (!rb) return;
    ...
}
```

**Non-fatal failures:** The CAN socket functions treat timestamp setup and
timeout configuration as non-fatal.  The socket remains open and functional
if these optional `setsockopt()` calls fail.

---

## 11. Configuration Reference

All options are set via the command line.  There is no configuration file.

```
Usage: canbus_monitor [OPTIONS] <interface>
```

| Option | Short | Default | Description |
|---|---|---|---|
| `--output <dir>` | `-o` | `./canbus_logs` | Log output directory |
| `--prefix <name>` | `-p` | Interface name | Log filename prefix |
| `--max-size <bytes>` | `-s` | 52 428 800 (50 MiB) | Rotate at this file size |
| `--rotations <n>` | `-r` | 10 | Number of rotated files to retain |
| `--timeout <ms>` | `-t` | 200 | Socket receive timeout in ms |
| `--filter <id:mask>` | `-f` | (none) | Hex CAN filter, repeatable up to 32× |
| `--count <n>` | `-n` | 0 (unlimited) | Stop after n frames |
| `--stats` | — | off | Print per-ID statistics table on exit |
| `--verbose` | `-v` | off | Print each frame to stdout |
| `--help` | `-h` | — | Show usage and exit |
| `--version` | — | — | Print version and exit |

**Filter format:** `<id>[:<mask>]` in hexadecimal, no `0x` prefix needed.

| Example | Effect |
|---|---|
| `-f 100:7FF` | Only ID 0x100 exactly |
| `-f 1F0:FF0` | IDs 0x1F0–0x1FF (low nibble wildcard) |
| `-f 1A0` | Only ID 0x1A0 (mask defaults to 0x7FF) |
| `-f 0:0` | Accept all frames (mask = 0 ≡ wildcard) |

Multiple `-f` filters are OR-combined by the kernel.

**Signals:**

| Signal | Effect |
|---|---|
| `SIGINT` (Ctrl-C) | Flush, print stats (if `--stats`), exit |
| `SIGTERM` | Same as SIGINT |
| `SIGHUP` | Flush current file and open a new one (log rotation) |

---

## 12. Test Suite

Tests use a vendored copy of the **Unity** unit test framework
(`tests/unity/`, MIT licence).  Unity provides assertion macros
(`TEST_ASSERT_*`) and a minimal test runner.

### Running tests

```bash
# After a successful build:
cd build && ctest --output-on-failure

# Or run individual executables:
./build/tests/test_ring_buffer
./build/tests/test_statistics
./build/tests/test_cli_parser
./build/tests/test_frame_logger
```

### Test coverage summary

#### `test_ring_buffer` — 10 tests

| Test | What it verifies |
|---|---|
| `test_create_returns_non_null` | Allocation succeeds |
| `test_capacity_rounded_to_power_of_two` | `rb_capacity() >= requested` |
| `test_empty_buffer_pop_returns_false` | Pop on empty buffer returns false |
| `test_push_pop_single_item` | Round-trip preserves id and data[] |
| `test_fifo_ordering` | Items dequeued in insertion order |
| `test_full_buffer_push_returns_false` | Push on full buffer returns false |
| `test_size_tracking` | `rb_size()` matches push/pop operations |
| `test_wrap_around_multiple_cycles` | Indices wrap correctly 3× capacity |
| `test_destroy_null_is_safe` | `rb_destroy(NULL)` does not crash |
| `test_zero_item_size_returns_null` | `rb_create(8, 0)` returns NULL |

#### `test_statistics` — 10 tests

| Test | What it verifies |
|---|---|
| `test_create_destroy` | Allocation and free without crash |
| `test_destroy_null_is_safe` | `stats_destroy(NULL)` does not crash |
| `test_aggregate_counts_single_frame` | 1 frame → total_frames=1, bytes=DLC |
| `test_aggregate_counts_multiple_frames` | 10 frames → correct totals |
| `test_dlc_min_max` | Varied DLCs → correct byte count sum |
| `test_multiple_unique_ids` | 16 different IDs → unique_ids=16 |
| `test_hash_table_grows_with_many_ids` | 300 IDs → correct count after resizes |
| `test_dropped_frame_counter` | `stats_increment_dropped()` × 3 |
| `test_log_error_counter` | `stats_increment_log_error()` × 1 |
| `test_same_id_repeated` | 50 frames same ID → correct totals |

#### `test_cli_parser` — 13 tests

| Test | What it verifies |
|---|---|
| `test_defaults_with_interface_only` | All defaults correct with `can0` only |
| `test_custom_output_directory` | `--output` sets `logger.base_path` |
| `test_custom_prefix` | `--prefix` sets `logger.prefix` |
| `test_prefix_defaults_to_interface` | Prefix = iface when `-p` omitted |
| `test_verbose_flag` | `--verbose` sets `cfg.verbose = true` |
| `test_stats_flag` | `--stats` sets `cfg.show_stats = true` |
| `test_frame_count_limit` | `--count 1000` sets `max_frames = 1000` |
| `test_timeout_override` | `--timeout 500` sets `timeout_ms = 500` |
| `test_single_filter` | `--filter 1F0:FF0` → id=0x1F0, mask=0xFF0 |
| `test_multiple_filters` | Three `--filter` entries all parsed |
| `test_filter_id_only_gets_default_mask` | No colon → mask defaults to 0x7FF |
| `test_max_size_option` | `--max-size 10485760` sets correct value |
| `test_rotations_option` | `--rotations 5` sets `max_rotations = 5` |

#### `test_frame_logger` — 8 tests

| Test | What it verifies |
|---|---|
| `test_create_and_destroy` | Logger lifecycle without crash |
| `test_creates_asc_file_on_open` | At least one `.asc` file created |
| `test_asc_header_written` | File contains `"date"` and `"base hex"` |
| `test_write_and_flush_produces_asc_line` | Frame 0x1A0 appears in log |
| `test_multiple_writes_all_flushed` | 20 frames → 20 `"Rx"` occurrences |
| `test_destroy_null_is_safe` | `frame_logger_destroy(NULL)` does not crash |
| `test_null_config_returns_null` | `frame_logger_create(NULL)` returns NULL |
| `test_rotation_creates_new_file` | 30 frames in a 256-byte file → ≥2 files |

The frame logger tests use `mkdtemp()` to create a unique temporary
directory per test and `rm -rf` it during teardown, so they leave no
artefacts on the filesystem.

### Unity framework

Unity is a minimal C unit test framework.  The key macros used:

| Macro | Checks |
|---|---|
| `TEST_ASSERT_NOT_NULL(p)` | `p != NULL` |
| `TEST_ASSERT_NULL(p)` | `p == NULL` |
| `TEST_ASSERT_TRUE(cond)` | `cond != 0` |
| `TEST_ASSERT_FALSE(cond)` | `cond == 0` |
| `TEST_ASSERT_EQUAL_INT(a, b)` | `(int)a == (int)b` |
| `TEST_ASSERT_EQUAL_UINT(a, b)` | `(unsigned)a == (unsigned)b` |
| `TEST_ASSERT_EQUAL_UINT64(a, b)` | `(uint64_t)a == (uint64_t)b` |
| `TEST_ASSERT_EQUAL_SIZE_T(a, b)` | `(size_t)a == (size_t)b` |
| `TEST_ASSERT_EQUAL_STRING(a, b)` | `strcmp(a, b) == 0` |
| `TEST_PASS()` | Always passes (used for NULL-safety tests) |

---

## 13. Scripts Reference

### `scripts/build.sh`

```bash
./scripts/build.sh [debug|release] [clean]
```

| Argument | Meaning |
|---|---|
| `debug` | `-DCMAKE_BUILD_TYPE=Debug` (ASAN + UBSan + `-g3`) |
| `release` | `-DCMAKE_BUILD_TYPE=Release` (`-O2 -DNDEBUG`) |
| `clean` | Delete `build/` before configuring |

Also passes `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` so editors can find the
compile database at `build/compile_commands.json`.

### `scripts/setup_vcan.sh`

Creates or destroys a virtual SocketCAN interface using the Linux `vcan`
kernel module.  Requires root (`sudo`).

```bash
sudo ./scripts/setup_vcan.sh up vcan0    # Create and bring up vcan0
sudo ./scripts/setup_vcan.sh down vcan0  # Tear down vcan0
```

**Under the hood:**

```bash
modprobe vcan                          # Load the vcan kernel module
ip link add dev vcan0 type vcan        # Create the interface
ip link set vcan0 up                   # Bring it up
```

Once up, test with `can-utils`:

```bash
# Send a test frame:
cansend vcan0 1A0#01020304

# Monitor all traffic:
candump vcan0

# Run the monitor:
./build/canbus_monitor vcan0
```

---

*End of INTERNALS.md*
