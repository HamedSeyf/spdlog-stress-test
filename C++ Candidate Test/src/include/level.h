#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "platform.h"
#include "test_base.h"


namespace level
{
    // PURPOSE: Concurrent atomic counter stress test.
    // Two threads share one atomic<int>:
    //   w — writer: increments g_level as fast as possible
    //   r — reader: samples g_level and logs whether the value is odd or even
    //
    // Tests:
    //   - fetch_add atomicity under concurrent read pressure
    //   - cache line ownership ping-pong between w and r
    //   - DEOS_SPIN_OR_SLEEP_MS behaviour under both stress and non-stress modes
    //
    // DEOS_CACHE_ALIGN: g_level gets its own cache line — prevents false sharing
    // with any adjacent variables. fetch_add from w won't invalidate other lines.
    //
    // Architecture note: w and r are internal to run() and managed here.
    // run() blocks until both threads exit — consistent with TestBase::run() contract.
    // A better approach for performance-sensitive use would be to return the threads
    // to the caller, but that would change the TestBase interface.

    DEOS_CACHE_ALIGN inline std::atomic<int> g_level{ 0 };

} // namespace level


// ============================================================
// LevelTest
// ============================================================
class LevelTest final : public TestBase
{
public:
    static constexpr const char* k_name = "level";
    const char* name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        // Extract raw observer pointer once — stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const logger_ptr = logger_.get();

        // For reusability in case run is expected to be called multiple times
        level::g_level.store(0, std::memory_order_relaxed);

        // I have intentionally left the logic here untouched so there are two separate threads created and run inside this run to demonstrate potentials of this run function.
        // This would result in "odd" and "even" logs not being consistent with each other as write and read are done in two separate threads.
        std::thread write_thread([&stop, stress]
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    level::g_level.fetch_add(1, std::memory_order_relaxed);

                    DEOS_SPIN_OR_SLEEP_MS(stress, 1);
                }
            });

        std::thread read_thread;
        try
        {
            read_thread = std::thread([&stop, stress, logger_ptr]
                {
                    while (!stop.load(std::memory_order_acquire))
                    {
                        const int level = level::g_level.load(std::memory_order_relaxed);

                        if (level & 1)
                        {
                            logger_ptr->info("odd");
                        }
                        else
                        {
                            logger_ptr->info("even");
                        }

                        DEOS_SPIN_OR_SLEEP_MS(stress, 1);
                    }
                });
        }
        catch (...)
        {
            // r failed to start — join write_thread before propagating
            write_thread.join();
            throw;
        }

        write_thread.join();
        read_thread.join();
    }
};
