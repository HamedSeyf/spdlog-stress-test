#pragma once

#include <atomic>
#include <memory>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include "platform.h"
#include "test_base.h"


namespace sink_callback
{

    class ReentrantSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
    {
    public:

        explicit ReentrantSink(std::shared_ptr<spdlog::logger> logger)
            : sinklogger_(std::move(logger)) {}

    protected:
        void sink_it_(const spdlog::details::log_msg& msg) override
        {
            sinklogger_->log(msg.level, "{}", msg.payload);

            sinklogger_->info("sink_it_ re-entry");
        }

        void flush_() override {}

    private:
        std::shared_ptr<spdlog::logger> sinklogger_;
    };
} // namespace sink_callback


// ============================================================
// SinkCallbackTest
// ============================================================
class SinkCallbackTest final : public TestBase
{
public:
    static constexpr std::string_view k_name = "sink_callback";
    std::string_view name() const noexcept override { return k_name; }

protected:

    void run(const std::atomic<bool>& stop, bool stress) override
    {
        if (!stress)
        {
            logger_->info("{} early exit in non-stress mode", name().data()); // name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data()
            return;
        }

        // may throw std::bad_alloc — propagates to start() then thread lambda
        auto sink = std::make_shared<sink_callback::ReentrantSink>(logger_);
        auto logger = std::make_shared<spdlog::logger>("reentrant", sink);

        while (!stop.load(std::memory_order_acquire))
        {
            logger->info("trigger");

            HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 100);
        }
    }
};
