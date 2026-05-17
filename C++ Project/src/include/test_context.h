#pragma once

#include <climits>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace test_context
{
    static constexpr std::string_view k_default_logfile_name = "output.log";
}

// ============================================================
// TestContext
// ============================================================
// Owns all runtime configuration for the test harness.
// Constructed once via TestContext::parse() in main.
// Passed by const ref to TestBase::configure() for each test.
//
// Arguments:
//   --seconds N          duration to run all tests (default: 5)
//   --stress             enable stress mode across all tests (default: off)
//   --loglevel LEVEL     trace/debug/info/warn/error/critical/off (default: info)
//   --filename PATH      output log file; if absent, no global file logger is shared between the tests. Each test would create its own log file.
//   --ignoretests A,B,C  comma-separated list of test names to skip (default: run all)
// ============================================================

struct TestContext
{
    int                                 seconds     = 5;
    bool                                stress      = false;
    spdlog::level::level_enum           log_level   = spdlog::level::level_enum::info;
    size_t                              queue_size  = 8192;  // async logger queue capacity in messages
    // [[C++ 17 : std::optional]]
    std::optional<std::string>          filename    = std::nullopt;         // std::nullopt = no global file logger between tests (main log still goes into "output.log")
    std::optional<int>                  max_threads = std::nullopt;
    std::unordered_set<std::string>     ignore_tests;     // empty = run all

    // Returns true if the named test should be included in current run
    bool should_run(std::string_view test_name) const
    {
        return ignore_tests.find(std::string(test_name)) == ignore_tests.end();
    }

    static TestContext parse(int argc, char** argv)
    {
        TestContext ctx;

        for (int i = 1; i < argc; ++i)
        {
            // --seconds N
            if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            {
                char* end = nullptr;
                // [[C++ 17 : if / switch with initializers]]
                if (const long val = std::strtol(argv[++i], &end, 10);
                    end != argv[i] && val > 0 && val <= INT_MAX)
                {
                    ctx.seconds = static_cast<int>(val);
                }
                else
                {
                    fprintf(stderr, "[args] invalid --seconds value '%s', using default %d\n",
                        argv[i], ctx.seconds);
                }
            }

            // --stress
            else if (std::strcmp(argv[i], "--stress") == 0)
            {
                ctx.stress = true;
            }

            // --loglevel LEVEL
            else if (std::strcmp(argv[i], "--loglevel") == 0 && i + 1 < argc)
            {
                // [[C++ 17 : if / switch with initializers]]
                if (const auto level = spdlog::level::from_str(argv[++i]);
                    level == spdlog::level::off && std::strcmp(argv[i], "off") != 0)
                {
                    fprintf(stderr, "[args] unknown --loglevel '%s', using info\n", argv[i]);
                }
                else
                {
                    ctx.log_level = level;
                }
            }

            // --queuesize N
            else if (std::strcmp(argv[i], "--queuesize") == 0 && i + 1 < argc)
            {
                char* end = nullptr;
                // [[C++ 17 : if / switch with initializers]]
                if (const long val = std::strtol(argv[++i], &end, 10);
                    end != argv[i] && val > 0)
                {
                    ctx.queue_size = static_cast<size_t>(val);
                }
                else
                {
                    fprintf(stderr, "[args] invalid --queuesize value '%s', using default %zu\n",
                        argv[i], ctx.queue_size);
                }
            }

            // --filename PATH
            else if (std::strcmp(argv[i], "--filename") == 0 && i + 1 < argc)
            {
                ctx.filename = argv[++i];
            }

            // --maxthreads maximum number of threads to be used by main.cpp
            else if (std::strcmp(argv[i], "--maxthreads") == 0 && i + 1 < argc)
            {
                char* end = nullptr;

                if (const long val = std::strtol(argv[++i], &end, 10);
                    end != argv[i] && val > 0 && val <= INT_MAX)
                {
                    ctx.max_threads = static_cast<int>(val);
                }
                else
                {
                    fprintf(stderr, "[args] invalid --maxthreads value '%s', using unlimited threads\n", argv[i]);
                }
            }

            // --ignoretests A,B,C
            else if (std::strcmp(argv[i], "--ignoretests") == 0 && i + 1 < argc)
            {
                std::stringstream ss(argv[++i]);
                std::string token;
                while (std::getline(ss, token, ','))
                {
                    if (!token.empty())
                    {
                        ctx.ignore_tests.insert(token);
                    }
                }
            }
        }

        return ctx;
    }

};
