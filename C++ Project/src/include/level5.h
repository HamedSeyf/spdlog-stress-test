#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "platform.h"
#include "test_base.h"

namespace level5
{

    class Level5
    {
    public:
        explicit Level5(const std::string& ss) : s(ss) {}
        explicit Level5(std::string&& ss)      : s(std::move(ss)) {}

        ~Level5() = default;

        void print(spdlog::logger* const logger_ptr) const
        {
            // Note: This log does not show up with project's default settings since log level is set to info by default (see TestContext::log_level)
            logger_ptr->debug("{}", s);
        }

    private:
        std::string s;
    };

} // namespace level5


// ============================================================
// Level5Test
// ============================================================
class Level5Test final : public TestBase
{
public:
    static constexpr std::string_view k_name = "level5";
    std::string_view name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        // Extract raw observer pointer once — stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const logger_ptr = logger_.get();

        level5::Level5 obj("level 5 logs");

        while (!stop.load(std::memory_order_acquire))
        {
            obj.print(logger_ptr);

            HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 50);
        }
    }
};
