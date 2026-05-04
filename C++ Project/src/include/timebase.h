#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <spdlog/spdlog.h>
#include "test_base.h"


// ============================================================
// TimebaseTest
// ============================================================

class TimebaseTest final : public TestBase
{
public:
    static constexpr std::string_view k_name = "timebase";
    std::string_view name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        // Extract raw observer pointer once — stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const logger_ptr = logger_.get();

        const auto sleep_duration = stress ? std::chrono::microseconds(100) : std::chrono::milliseconds(1);

        const int log_every = stress ? 64 : 16;

        int       sample_count = 0;
        long long sum_us = 0;

        while (!stop.load(std::memory_order_acquire))
        {
            const auto t1 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(sleep_duration);
            const auto t2 = std::chrono::steady_clock::now();

            const long long us =
                std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

            sum_us += us;
            ++sample_count;

            if (sample_count >= log_every)
            {
                const long long avg_us = sum_us / sample_count;
                logger_ptr->info("latency_avg={} us samples={}", avg_us, sample_count);

                sample_count = 0;
                sum_us = 0;
            }
        }
    }
};