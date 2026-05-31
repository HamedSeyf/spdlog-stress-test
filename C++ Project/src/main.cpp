#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "include/test_context.h"
#include "include/test_base.h"
#include "include/thread_pool.h"
#include "include/platform.h"

#include "include/level.h"
#include "include/level5.h"
#include "include/string_uaf.h"
#include "include/sink_callback.h"
#include "include/diagnostics.h"
#include "include/timebase.h"
#include "include/concurrent_metrics.h"
#include "include/producer_consumer_queue.h"
// Just to get the file compiled although they are not used in the solution
#include "include/non_test/notification_manager.h"


// --------------------------------------------------------
// Test registry — name known statically, no instance needed.
// One entry per test. To add a new test: one line here.
// --------------------------------------------------------
struct TestEntry {
    std::string_view                             name;
    std::function<std::unique_ptr<TestBase>()>   create;
};

static const std::vector<TestEntry> k_registry = {
    { StringUafTest::k_name,                [] { return std::make_unique<StringUafTest>();              } },
    { TimebaseTest::k_name,                 [] { return std::make_unique<TimebaseTest>();               } },
    { LevelTest::k_name,                    [] { return std::make_unique<LevelTest>();                  } },
    { DiagnosticsTest::k_name,              [] { return std::make_unique<DiagnosticsTest>();            } },
    { SinkCallbackTest::k_name,             [] { return std::make_unique<SinkCallbackTest>();           } },
    { Level5Test::k_name,                   [] { return std::make_unique<Level5Test>();                 } },
    { ConcurrentMetricsTest::k_name,        [] { return std::make_unique<ConcurrentMetricsTest>();      } },
    { ProducerConsumerQueueTest::k_name,    [] { return std::make_unique<ProducerConsumerQueueTest>();  } },
};

int main(int argc, char** argv)
{
    try
    {
        // Error handler: spdlog cannot log from inside this callback — it may be in a broken state. Example of catches: queue overflow (overrun_oldest drops), sink write failures, flush errors, thread pool exhaustion etc.
        spdlog::set_error_handler([](const std::string& msg)
            {
                fprintf(stderr, "[spdlog internal error] %s\n", msg.c_str());
            });

        // Parse all arguments into TestContext.
        TestContext ctx = TestContext::parse(argc, argv);

        // Main / Shared Logger setup.
        auto logger_thread_pool = std::make_shared<spdlog::details::thread_pool>(ctx.queue_size, 1);

        std::shared_ptr<spdlog::async_logger> main_logger = Helpers::make_logger(
            ctx.filename.value_or(test_context::k_default_logfile_name.data()), // k_default_logfile_name is a string_view literal and always guaranteed to be null terminated so it's safe to use data() directly
            "main",
            logger_thread_pool,
            ctx.log_level,
            ctx.stress ? spdlog::async_overflow_policy::overrun_oldest : spdlog::async_overflow_policy::block
        );

        // Phase 1 — create only non-ignored tests
        std::vector<std::unique_ptr<TestBase>> tests;
        tests.reserve(k_registry.size());
        // [[C++ 17 : Structured bindings]]
        for (const auto& [test_name, create] : k_registry)
        {
            if (!ctx.should_run(test_name))
            {
                main_logger->info("Skipping test '{}'", test_name);
                continue;  // factory never called — zero allocation
            }
            tests.push_back(create());
        }

        // Phase 2 — Configure all tests.
        for (auto& test : tests)
        {
            if (ctx.filename)
            {
                test->configure(main_logger);
            }
            else
            {
                test->configure(ctx);
            }
        }

        // Phase 3 — Start non-ignored tests.
        HAMEDSEYF_CACHE_ALIGN std::atomic<bool> stop{ false };
        HAMEDSEYF_CACHE_ALIGN std::atomic<int> active_tests_count{ 0 };

        ThreadPool test_thread_pool(ctx.max_threads);

        for (auto& test : tests)
        {
            active_tests_count.fetch_add(1, std::memory_order_relaxed);

            test_thread_pool.Submit([&active_tests_count, &stop, stress = ctx.stress, test_object = test.get()]
                {
                    try
                    {
                        test_object->start(stop, stress);
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw: %s\n", test_object->name().data(), e.what()); // same as k_default_logfile_name, name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data() on the returned value
                    }
                    catch (...)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", test_object->name().data()); // same as k_default_logfile_name, name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data() on the returned value
                    }

                    active_tests_count.fetch_sub(1, std::memory_order_release);
                });
        }

        // Main / Heartbeat loop — runs for ctx.seconds OR until all tests exit, whichever comes first.
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(ctx.seconds);

        while (std::chrono::steady_clock::now() < end)
        {
            if (active_tests_count.load(std::memory_order_acquire) == 0)
            {
                main_logger->info("all tests finished early");
                break;
            }

            main_logger->info("heartbeat");

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Phase 4 — Signal stop, join all threads via threadpool, shut down logger.
        stop.store(true, std::memory_order_release);
        test_thread_pool.Wait();

        tests.clear();                    // per-test loggers and pools freed
        logger_thread_pool.reset();       // main pool background thread joins cleanly

        spdlog::shutdown();

        return 0;
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[fatal] unhandled exception in main: %s\n", e.what());
        return 1;
    }
    catch (...)
    {
        fprintf(stderr, "[fatal] unknown exception in main\n");
        return 1;
    }
}
