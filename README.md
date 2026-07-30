# spdlogger Logging Stress Test

A multithreaded C++ stress-test harness for spdlog, focused on async logging, threading correctness, lifecycle management, and runtime diagnostics.

This project demonstrates modern C++ techniques applied to real-world systems problems such as safe shutdown, contention, configurability, and platform-aware performance.

---

## Highlights

- Modern C++17/20 usage: `std::variant`, `std::any`, `std::optional`, `std::string_view`, structured bindings  
- Multithreaded async logging with explicit lifecycle ownership  
- Deadlock-safe synchronization using `std::scoped_lock`  
- Per-test logger isolation with optional shared logger  
- CLI-driven runtime configuration  
- Platform-aware optimizations (`HAMEDSEYF_CACHE_ALIGN`, `HAMEDSEYF_CPU_RELAX`, `HAMEDSEYF_SPIN_OR_SLEEP_MS`)  

---

## Architecture

The system follows a clear lifecycle driven by `main()`:

construct → configure → run

Each test derives from a common `TestBase` interface and executes independently, ensuring isolation and predictable behavior.

### Core Components

- **TestBase**
  - Defines lifecycle (`configure`, `run`, `start`)
  - Owns thread execution and logging boundaries

- **TestContext**
  - Parses CLI arguments
  - Produces a fully resolved runtime configuration

- **Logger Factory (`Helpers::make_logger`)**
  - Creates async loggers with configurable queue size and overflow policy
  - Supports both shared and per-test logging modes

- **Platform Layer (`platform.h`)**
  - Encapsulates CPU hints, cache alignment, and platform differences
  - Supports MSVC, GCC, Clang across x86 and ARM64

---

## Concurrency Design

The project demonstrates correct multi-mutex synchronization using `std::scoped_lock`.

Two internal threads operate on shared state:

collector → locks (metrics → buffer)  
reporter  → locks (buffer → metrics)

These opposite acquisition orders would normally introduce deadlock risk.  
`std::scoped_lock` prevents this by internally using `std::lock`, ensuring safe acquisition regardless of order.

---

## Logging Model

Two modes are supported:

### Shared logger

--filename output.log

All tests write to a single async logger.

### Per-test loggers (default)

(no --filename)

Each test writes to:

<testname>.log

Logger names are automatically embedded via spdlog’s `%n` pattern.

---

## CLI Arguments

| Argument | Default | Description |
|---|---|---|
| `--seconds N` | `5` | Duration to run tests |
| `--stress` | off | Enable stress mode (higher frequency, spin hints) |
| `--loglevel LEVEL` | `info` | `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off` |
| `--filename PATH` | _(none)_ | Optional shared output log file |
| `--queuesize N` | `8192` | Async logger queue capacity |
| `--ignoretests A,B` | _(none)_ | Comma-separated test names to skip |

---

## Examples

# Default run (per-test logs)
./spdlogger

# Shared log file
./spdlogger --filename output.log

# Stress mode
./spdlogger --seconds 10 --stress

# Skip selected tests
./spdlogger --ignoretests level,sink_callback

# Debug-level output
./spdlogger --loglevel debug

---

## Platform Support

| Platform | Architecture | Status |
|---|---|---|
| Windows (MSVC) | x64 / ARM64 | ✅ |
| Linux (GCC/Clang) | x86_64 / ARM64 | ✅ |
| macOS | ARM64 | ✅ |
| Consoles (PS5 / Xbox Series X) | ARM64 | ⚠️ requires path adjustments |

---

## Dependencies

- spdlog (header-only async logging library)  
- C++17 or later  

---

## Key Engineering Considerations

- Safe async logger shutdown sequencing  
- Avoidance of false sharing via cache alignment  
- Efficient atomic usage (`memory_order` tuning)  
- Exception-safe thread lifecycle management  
- Zero-cost abstractions in hot loops  

---

## Build

cmake -S . -B build

cmake --build build

---

## Summary

This project focuses on building a robust, configurable, and high-performance logging test harness using modern C++ techniques, with particular attention to concurrency correctness and system-level behavior under stress.
