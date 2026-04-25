#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "test_context.h"


namespace Helpers
{

    // Helper function. It would be better to have a utils file but for sakes of this test and keeping changes to the minimum we are adding this 1 file here
    inline std::shared_ptr<spdlog::async_logger> make_logger(const std::string& filename, const std::string& logger_name, const std::shared_ptr<spdlog::details::thread_pool>& thread_pool, spdlog::level::level_enum log_level, spdlog::async_overflow_policy overflow_policy)
    {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;

        try
        {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename, true);
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[fatal] failed to open log file '%s': %s\n", filename.c_str(), e.what());
            throw;  // propagates to main's top-level catch
        }

        sink->set_pattern("[%H:%M:%S.%e] [%n] [%l] %v");

        auto logger = std::make_shared<spdlog::async_logger>(
            logger_name,
            spdlog::sinks_init_list{ sink },
            thread_pool,
            overflow_policy);

        logger->set_level(log_level);

        return logger;
    }
}


// ============================================================
// TestBase
// ============================================================
// Abstract base for all test modules.
//
// Lifecycle (three phases, driven by main):
//
//   Phase 1 — construct
//     Each test is default-constructed. No logger, no stop, nothing live yet.
//     name() is pure virtual and safe to call after construction.
//
//   Phase 2 — configure via configure_with_logger or configure_with_context
//
//   Phase 3 — start(stop, stress)
//     Starts the test. Usually blocks the caller thread so should be called inside an async thread (main.cpp handles this in this case)
//     Internally calls run() which each subclass implements.
//     Logs "started" and "finished" automatically around run().
//     At the end, flushes the logger in case this has been the last standing test or has its own dedicated logger.
//
// Notes:
//   - run() is protected — only callable via start().
//   - logger_ is protected — subclasses may use it directly inside run().
//   - The hot loop inside run() sees stop and stress as direct parameters,
//     identical in cost to the original captured-reference/value lambdas.
// ============================================================

class TestBase
{
public:
    virtual const char* name() const noexcept = 0;

    // When this test should share log file with other tests or main
    void configure_with_logger(const std::shared_ptr<spdlog::logger>& logger_override)
    {
        // universal logger provided — all tests write under the same logger name -> This is for performance sakes to avoid calling name() per each test log in the loops.
        logger_ = logger_override;

        logger_->info("Test configured: {}", name());
    }

    // When this test should have its own dedicated logger, pool and file output
    void configure_with_context(const TestContext& ctx)
    {
        try
        {
            logger_thread_pool = std::make_shared<spdlog::details::thread_pool>(
                ctx.queue_size,
                1
            );

            logger_ = Helpers::make_logger(
                std::string(name()) + ".log",
                name(),
                logger_thread_pool,
                ctx.log_level,
                ctx.stress ? spdlog::async_overflow_policy::overrun_oldest : spdlog::async_overflow_policy::block
            );
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[fatal] failed to configure test '%s': %s\n", name(), e.what());
            throw;
        }

        logger_->info("Test configured: {}", name());
    }

    void start(const std::atomic<bool>& stop, bool stress)
    {
        if (!logger_)
        {
            fprintf(stderr, "[fatal] test '%s' started without being configured\n", name());
            return;
        }

        logger_->info("Test started: {}", name());
        run(stop, stress);
        logger_->info("Test finished: {}", name());

        logger_->flush();
    }

protected:
    virtual void run(const std::atomic<bool>& stop, bool stress) = 0;

    // Available to subclasses for logging inside run()
    std::shared_ptr<spdlog::logger> logger_;

private:
    // This thread pool is valid only when test is configured via configure_with_context; i.e. only when this test case has created and owns its own logger and threadpool
    std::shared_ptr<spdlog::details::thread_pool> logger_thread_pool;
};
