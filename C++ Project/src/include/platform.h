#pragma once

#include <chrono>
#include <thread>

// ============================================================
// HAMEDSEYF_CACHE_ALIGN
// ============================================================
// Aligns a variable to the platform's cache line size to prevent
// false sharing between independently accessed atomic variables.
//
// Usage:
//   HAMEDSEYF_CACHE_ALIGN inline std::atomic<int> g_level{ 0 };
//
// Platform behaviour:
//   C++17 hardware_destructive_interference_size — compile-time exact value
//   ARM/MIPS 32-bit                              — 32 bytes
//   x86, x86_64, ARM64 fallback                  — 64 bytes
//   128-byte platforms (PS5, Xbox, Apple Silicon) — covered by interference_size
// 
// ============================================================

#ifdef __cpp_lib_hardware_interference_size
	#include <new>
	#define HAMEDSEYF_CACHE_ALIGN alignas(std::hardware_destructive_interference_size)
#elif defined(__arm__) || defined(__mips__)
	#define HAMEDSEYF_CACHE_ALIGN alignas(32)
#else
	#define HAMEDSEYF_CACHE_ALIGN alignas(64)
#endif


// ============================================================
// HAMEDSEYF_CPU_RELAX()
// ============================================================
// Emits a lightweight CPU spin-wait hint for the current architecture.
//
// Purpose:
//   Used inside tight polling / retry / spin loops to reduce power waste
//   and improve behavior under contention without actually sleeping or
//   yielding the thread to the OS scheduler.
//
// Behaviour:
//   x86/x86_64: emits PAUSE
//   ARM64:      emits YIELD
//   Other:      no-op
//
// Notes:
//   - This is NOT std::this_thread::yield().
//   - This does NOT sleep.
//   - This does NOT deschedule the thread.
//   - It only gives the CPU a spin-loop hint.
//
// Include policy:
//   - Intrinsic headers are included here because the intrinsic itself
//     requires them.
//
// ============================================================

#if defined(__x86_64__) || defined(_M_X64)
	#if defined(_MSC_VER)
		#include <intrin.h>
		#define HAMEDSEYF_CPU_RELAX() _mm_pause()
	#else
		#include <x86intrin.h>
		#define HAMEDSEYF_CPU_RELAX() __builtin_ia32_pause()
	#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
	#if defined(_MSC_VER)
		#include <intrin.h>
		#define HAMEDSEYF_CPU_RELAX() __yield()
	#else
		#define HAMEDSEYF_CPU_RELAX() asm volatile("yield")
	#endif
#else
	#define HAMEDSEYF_CPU_RELAX() ((void)0)
#endif


// ============================================================
// HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, sleep_ms)
// ============================================================
// In stress mode:
//   Executes HAMEDSEYF_CPU_RELAX().
//   This keeps the thread active and maximizes hot-loop pressure while
//   still giving the processor a spin hint.
//
// In non-stress mode:
//   Sleeps for sleep_ms milliseconds.
//
// Parameters:
//   stress   — bool, true enables HAMEDSEYF_CPU_RELAX path
//   sleep_ms — milliseconds to sleep in non-stress mode
//
// Usage:
//   HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 1);    // stress=CPU hint, non-stress=1ms sleep
//   HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 10);   // stress=CPU hint, non-stress=10ms sleep
// ============================================================

#define HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, sleep_ms)                                          \
    do {                                                                               \
        if ((stress)) {                                                                \
            HAMEDSEYF_CPU_RELAX();                                                          \
        } else {                                                                       \
            std::this_thread::sleep_for(std::chrono::milliseconds((sleep_ms)));       \
        }                                                                              \
    } while (0)
