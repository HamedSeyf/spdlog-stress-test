#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <variant>
#include <filesystem>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "test_context.h"


namespace Helpers
{

    // Helper function. It would be better to have a utils file but for sakes of this test and keeping changes to the minimum we are adding this 1 file here
    inline std::shared_ptr<spdlog::async_logger> makeLogger(std::string_view filename, std::string_view loggerName, const std::shared_ptr<spdlog::details::thread_pool>& threadPool, spdlog::level::level_enum logLevel, spdlog::async_overflow_policy overflowPolicy)
    {
        const std::filesystem::path logPath(filename);

        if (const auto parent = logPath.parent_path(); !parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);   // non-throwing; ec checked below
            if (ec)
            {
                fprintf(stderr, "[warn] could not create log dir '%s': %s\n", parent.string().c_str(), ec.message().c_str());
            }
        }

        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;

        try
        {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(std::string(filename), true);
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[fatal] failed to open log file '%s': %s\n", std::string(filename).c_str(), e.what());
            throw;  // propagates to main's top-level catch
        }

        sink->set_pattern("[%H:%M:%S.%e] [%n] [%l] %v");

        auto logger = std::make_shared<spdlog::async_logger>(
            std::string(loggerName),
            spdlog::sinks_init_list{ sink },
            threadPool,
            overflowPolicy);

        logger->set_level(logLevel);

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

    virtual ~TestBase() = default;

    // shared mode — borrow an existing logger
    // owned mode — spin up own pool + file
    // [[C++ 17 : std::variant]]
    using LogConfig = std::variant<std::shared_ptr<spdlog::logger>, TestContext> ;

    // [[C++ 17 : std::string_view]]
    virtual std::string_view name() const noexcept = 0;

    void configure(const LogConfig& config)
    {
        // [[C++ 17 : std::variant]]
        std::visit([this](const auto& cfg)
            {
                using T = std::decay_t<decltype(cfg)>;

                // [[C++ 17 : if constexpr]]
                if constexpr (std::is_same_v<T, std::shared_ptr<spdlog::logger>>)
                {
                    // universal logger provided — all tests write under the same logger name -> This is for performance sakes to avoid calling name() per each test log in the loops.
                    logger_ = cfg;
                }
                else if constexpr (std::is_same_v<T, TestContext>)
                {
                    // When this test should have its own dedicated logger, pool and file output
                    try
                    {
                        loggerThreadPool_ = std::make_shared<spdlog::details::thread_pool>(
                            cfg.queueSize,
                            1
                        );

                        logger_ = Helpers::makeLogger(
                            std::string(name()) + ".log",
                            name(),
                            loggerThreadPool_,
                            cfg.logLevel,
                            cfg.stress ? spdlog::async_overflow_policy::overrun_oldest : spdlog::async_overflow_policy::block
                        );
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[fatal] failed to configure test '%s': %s\n", name().data(), e.what()); // name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data()
                        throw;
                    }
                }

                logger_->info("Test configured: {}", name().data());
            }
        , config);
    }

    void start(const std::atomic<bool>& stop, bool stress)
    {
        if (!logger_)
        {
            fprintf(stderr, "[fatal] test '%s' started without being configured\n", name().data()); // name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data()
            return;
        }

        logger_->info("Test started: {}", name().data());
        run(stop, stress);
        logger_->info("Test finished: {}", name().data());

        logger_->flush();
    }

protected:
    virtual void run(const std::atomic<bool>& stop, bool stress) = 0;

    // Available to subclasses for logging inside run()
    std::shared_ptr<spdlog::logger> logger_;

private:
    // This thread pool is valid only when test is configured via configure_with_context; i.e. only when this test case has created and owns its own logger and threadpool
    std::shared_ptr<spdlog::details::thread_pool> loggerThreadPool_;
};
