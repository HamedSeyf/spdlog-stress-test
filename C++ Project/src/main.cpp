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

#include "include/level.h"
#include "include/level5.h"
#include "include/string_uaf.h"
#include "include/sink_callback.h"
#include "include/diagnostics.h"
#include "include/timebase.h"
#include "include/concurrent_metrics.h"
#include "include/producer_consumer_queue.h"
#include "include/ride_state_manager.h"
// Just to get the file compiled although they are not used in the solution
#include "include/non_test/expiring_cache.h"

#include "hamed_common/platform.h"
#include "hamed_common/sorts.h"


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
    { RideStateManager::k_name,             [] { return std::make_unique<RideStateManager>();  } },
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
        auto loggerThreadPool = std::make_shared<spdlog::details::thread_pool>(ctx.queueSize, 1);

        std::shared_ptr<spdlog::async_logger> mainLogger = Helpers::makeLogger(
            ctx.filename.value_or(test_context::k_defaultLogfileName.data()), // k_defaultLogfileName is a string_view literal and always guaranteed to be null terminated so it's safe to use data() directly
            "main",
            loggerThreadPool,
            ctx.logLevel,
            ctx.stress ? spdlog::async_overflow_policy::overrun_oldest : spdlog::async_overflow_policy::block
        );

        // Phase 1 — create only non-ignored tests
        std::vector<std::unique_ptr<TestBase>> tests;
        tests.reserve(k_registry.size());
        // [[C++ 17 : Structured bindings]]
        for (const auto& [testName, create] : k_registry)
        {
            if (!ctx.shouldRun(testName))
            {
                mainLogger->info("Skipping test '{}'", testName);
                continue;  // factory never called — zero allocation
            }
            tests.push_back(create());
        }

        // Phase 2 — Configure all tests.
        for (auto& test : tests)
        {
            if (ctx.filename)
            {
                test->configure(mainLogger);
            }
            else
            {
                test->configure(ctx);
            }
        }

        // Phase 3 — Start non-ignored tests.
        HAMEDSEYF_CACHE_ALIGN std::atomic<bool> stop{ false };
        HAMEDSEYF_CACHE_ALIGN std::atomic<int> activeTestsCount{ 0 };

        ThreadPool testThreadPool(ctx.maxThreads);

        for (auto& test : tests)
        {
            activeTestsCount.fetch_add(1, std::memory_order_relaxed);

            testThreadPool.submit([&activeTestsCount, &stop, stress = ctx.stress, testObject = test.get()]
                {
                    try
                    {
                        testObject->start(stop, stress);
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw: %s\n", testObject->name().data(), e.what()); // same as k_defaultLogfileName, name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data() on the returned value
                    }
                    catch (...)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", testObject->name().data()); // same as k_defaultLogfileName, name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data() on the returned value
                    }

                    activeTestsCount.fetch_sub(1, std::memory_order_release);
                });
        }

        // Main / Heartbeat loop — runs for ctx.seconds OR until all tests exit, whichever comes first.
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(ctx.seconds);

        while (std::chrono::steady_clock::now() < end)
        {
            if (activeTestsCount.load(std::memory_order_acquire) == 0)
            {
                mainLogger->info("all tests finished early");
                break;
            }

            mainLogger->info("heartbeat");

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Phase 4 — Signal stop, join all threads via threadpool, shut down logger.
        stop.store(true, std::memory_order_release);
        testThreadPool.wait();

        tests.clear();                    // per-test loggers and pools freed
        loggerThreadPool.reset();       // main pool background thread joins cleanly

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
