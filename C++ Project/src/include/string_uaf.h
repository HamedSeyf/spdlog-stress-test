#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <string>

#include <spdlog/spdlog.h>

#include "platform.h"
#include "test_base.h"

namespace string_uaf
{
    namespace detail
    {
        // thread_local RNG: each thread has its own seeded mt19937 instance.
        // Zero contention — no locks, no shared state, no std::rand() global mutation.
        // Constructed once per thread on first call, reused on all subsequent calls.
        inline uint32_t next_rand()
        {
            thread_local std::mt19937 rng{ std::random_device{}() };
            thread_local std::uniform_int_distribution<uint32_t> dist;
            return dist(rng);
        }
    } // namespace detail

    struct DeferredLog
    {
        char data[32];
        int  data_len = 0;

        void set()
        {
            data_len = std::snprintf(data, sizeof(data),
                "deferred=uaf:%u", detail::next_rand());
        }

        void emit(spdlog::logger* const log) const
        {
            // [[C++ 17 : std::string_view]]
            log->info("{}", std::string_view(data, static_cast<size_t>(data_len)));
        }
    };

} // namespace string_uaf


// ============================================================
// StringUafTest
// ============================================================
class StringUafTest final : public TestBase
{
public:
    static constexpr std::string_view k_name = "string_uaf";
    std::string_view name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        // Extract raw observer pointer once — stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const logger_ptr = logger_.get();

        string_uaf::DeferredLog d;  // hoisted — buffer capacity survives across iterations

        while (!stop.load(std::memory_order_acquire))
        {
            d.set();

            std::this_thread::sleep_for(std::chrono::microseconds(stress ? 50 : 500));

            d.emit(logger_ptr);

            // Loop throttle: in stress mode emit CPU hint, in non-stress sleep 10ms.
            HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 10);
        }
    }
};
