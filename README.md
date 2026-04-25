# DeOS Logging Stress Test — Code Review & Refactor

This repository contains a multithreaded C++ stress test harness for [spdlog](https://github.com/gabime/spdlog), originally provided as a code review exercise. The original code contained a number of correctness bugs, architectural issues, and performance problems. This document summarises the high-level findings and the redesign approach.

> **Full detailed issue breakdown** — every bug, root cause, fix, and before/after comparison is captured in [`Code_Review_Results.xlsx`](Code_Review_Results.xlsx) at the root of this repository. The items listed below are high-level abstracts only.

---

> **Running the refactored project:** The original behaviour of writing all logs to a single `output.log` file is preserved but must now be explicitly requested via the `--filename` argument. To replicate the original default behaviour pass the following command line argument:
> ```
> --filename "output.log"
> ```
> Without `--filename`, each test creates its own individual log file named `<testname>.log`.

---

## Original Code — Issues Found

### Correctness / Crashes

- **`shutdown.h` called `spdlog::shutdown()` while other threads were still actively logging** — guaranteed crash under stress, producing a UAF on the async thread pool's internal atomics (`__iso_volatile_load32`)
- **No shutdown sequencing guarantee** — `spdlog::shutdown()` could fire before threads fully exited (TOCTOU window between `stop.load()` returning false and the subsequent `spdlog::info()` call completing)
- **`shutdown::run()` owned global infrastructure teardown** — violated single-ownership principle; `main()` should be the sole owner of logger lifecycle

### Architecture / Modularity

- **Code lacked modularity** — tests were heavily interconnected through shared writes to `stop` and shared global logger lifecycle; one module's behaviour could silently affect all others
- **`Level5` embedded inside `level.h`** despite being entirely unrelated to the atomic counter test — needed its own standalone file and test case
- **No exception handling anywhere** — `std::thread` construction failures, file open failures, and `bad_alloc` all propagated silently to `std::terminate` with no diagnostic output

### Performance

- **Default `seq_cst` memory ordering used on all atomics** — unnecessarily strong for plain cancellation flags, particularly costly on ARM64 where it emits a full `DMB ISH` pipeline stall per iteration rather than the lighter `LDAR` of `memory_order_acquire`
- **No cache line alignment on shared atomics** — false sharing between `g_level`, `stop`, and adjacent variables caused unnecessary cache invalidation across cores on every write
- **No raw pointer extraction before hot loops** — `shared_ptr` member access prevented the compiler from promoting the logger pointer to a register across loop iterations

### Design / Configuration

- **Project used the global spdlog default logger** — no per-test log attribution, shared lifecycle risk, no isolation between tests; all output was indistinguishable without reading message text
- **Project not configurable through CLI arguments** — log level, output filename, queue size, and test selection were all hardcoded
- **Different compile architectures not considered** — no handling for x86 vs ARM64 intrinsics, cache line size differences across platforms (PS5, Xbox Series X, and Apple Silicon use 128-byte cache lines vs 64-byte on x86), or MSVC vs GCC/Clang compiler divergences

---

## Refactored Architecture

The refactored solution introduces a clean three-phase lifecycle driven by `main()` — construct, configure, run — replacing the original ad-hoc thread spawning with a structured `TestBase` abstract base class that all six test modules subclass. The virtual dispatch overhead is negligible in practice: vtable lookups are limited to one or two calls per test lifetime (at `configure()` and `start()`) and never appear inside hot loops, which retain identical cost to the original unarchitectured lambdas.

Two new files anchor the architecture: `test_context.h` owns all runtime configuration parsed from CLI arguments (`--seconds`, `--stress`, `--loglevel`, `--filename`, `--queuesize`, `--ignoretests`), and `test_base.h` defines the shared lifecycle contract alongside a `Helpers::make_logger` factory that wires each test to its own named async logger. `platform.h` was introduced to centralise all platform-specific definitions — cache line alignment (`DEOS_CACHE_ALIGN`), CPU spin hints (`DEOS_CPU_RELAX`), and the `DEOS_SPIN_OR_SLEEP_MS` macro — covering x86, ARM64, MSVC, GCC, and Clang across Windows, Linux, macOS, and console targets. `Level5` was extracted from `level.h` into its own standalone `level5.h` and `Level5Test` subclass.

Each test now logs a structured start and finish message automatically via `TestBase::start()`, with an explicit `flush()` before thread exit to guarantee all pending async messages reach disk before `spdlog::shutdown()` fires. A layered exception handling strategy was added: a top-level `try/catch` in `main()` catches all synchronous setup failures (logger creation, thread pool allocation, file open errors) with a descriptive `stderr` message and clean exit, while a per-thread `try/catch` inside each thread lambda catches any exception escaping `run()` and guarantees `active_tests_count` always decrements correctly — preventing the heartbeat loop from hanging indefinitely on a test that threw.

The test registry filters by name before construction, meaning ignored tests are never allocated. The shared spdlog global logger is replaced by an explicitly owned `main_logger` instance — tests either share it when a `--filename` is provided, or each create their own individually named logger writing to a per-test `.log` file, with the logger name appearing automatically as a prefix on every log entry via spdlog's `%n` pattern token at zero per-call cost.

---

## CLI Arguments

| Argument | Default | Description |
|---|---|---|
| `--seconds N` | `5` | Duration to run all tests |
| `--stress` | off | Enable stress mode (CPU hints instead of sleeps, higher sample rates) |
| `--loglevel LEVEL` | `info` | `trace` / `debug` / `info` / `warn` / `error` / `critical` / `off` |
| `--filename PATH` | `output.log` | Output log file; all tests share one logger writing here |
| `--queuesize N` | `8192` | Async logger queue capacity (rounded to next power of two) |
| `--ignoretests A,B` | _(none)_ | Comma-separated list of test names to skip |

### Examples

```powershell
# Default run
.\DeOS_Test.exe

# Stress mode, 10 seconds
.\DeOS_Test.exe --seconds 10 --stress

# Skip flooding tests for clean diagnostic output
.\DeOS_Test.exe --seconds 5 --ignoretests level,sink_callback

# See Level5 output (logged at debug level)
.\DeOS_Test.exe --loglevel debug --seconds 5

# Per-test log files instead of shared output
.\DeOS_Test.exe --filename ""

# Large queue to handle stress mode without drops
.\DeOS_Test.exe --stress --seconds 10 --queuesize 65536

# Skip all tests (verify harness only)
.\DeOS_Test.exe --ignoretests string_uaf,timebase,level,shutdown,sink_callback,level5
```

---

## Platform Support

| Platform | Architecture | Status |
|---|---|---|
| Windows (MSVC) | x64 | ✅ Full support |
| Windows (MSVC) | ARM64 | ✅ Full support (`__yield()`, `_ReadStatusReg`) |
| Windows (MinGW) | x64 | ✅ Full support |
| Linux (GCC/Clang) | x86_64 | ✅ Full support |
| Linux (GCC/Clang) | ARM64 | ✅ Full support |
| macOS | Apple Silicon (ARM64) | ✅ Compiles — note 128-byte cache lines covered by `hardware_destructive_interference_size` |
| PS5 / Xbox Series X | ARM64 | ✅ Compiles — filesystem paths may require platform-specific configuration |
| 32-bit ARM (ARMv7) | ARM | ✅ Compiles — `CPU_RELAX` is no-op (no YIELD on ARMv7) |

---

## Dependencies

- [spdlog](https://github.com/gabime/spdlog) — header-only async logging library
- C++17 or later
