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
        // Extract raw observer pointer once � stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const loggerPtr = logger_.get();

        const auto sleepDuration = stress ? std::chrono::microseconds(100) : std::chrono::milliseconds(1);

        const int logEvery = stress ? 64 : 16;

        int       sampleCount = 0;
        long long sumUs = 0;

        while (!stop.load(std::memory_order_acquire))
        {
            const auto t1 = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(sleepDuration);
            const auto t2 = std::chrono::steady_clock::now();

            const long long us =
                std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

            sumUs += us;
            ++sampleCount;

            if (sampleCount >= logEvery)
            {
                const long long avgUs = sumUs / sampleCount;
                loggerPtr->info("latency_avg={} us samples={}", avgUs, sampleCount);

                sampleCount = 0;
                sumUs = 0;
            }
        }
    }
};