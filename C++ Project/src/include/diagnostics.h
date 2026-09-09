#pragma once
#include <any>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

#include "hamed_common/platform.h"

#include "test_base.h"

namespace diagnostics
{
    // Heterogeneous metric value: counters are int64, rates are double, labels are string
    using MetricValue = std::variant<int64_t, double, std::string>;

    // Formats any MetricValue to string via std::visit + if constexpr
    inline std::string formatMetric(const MetricValue& v)
    {
        return std::visit([](const auto& val) -> std::string
            {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, int64_t>)
                {
                    return std::to_string(val);
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return std::to_string(val);
                }
                else
                {
                    return val;
                }
            }, v);
    }

    struct ReportSummary
    {
        int64_t iterations = 0;
        int64_t metricCount = 0;
    };
} // namespace diagnostics


// ============================================================
// DiagnosticsTest
// ============================================================
// Two internal threads share metricsMutex_ and logBufferMutex_:
//
//   collector � natural order: metrics_ first, then logBuffer_
//   reporter  � natural order: logBuffer_ first, then metrics_
//
// These opposite natural orderings would deadlock if each thread
// acquired the two mutexes with separate lock_guards.
// std::scoped_lock calls std::lock() internally, which is deadlock-free
// regardless of the listing order supplied by the caller.
//
// This is the canonical use case for std::scoped_lock over lock_guard.
// ============================================================
class DiagnosticsTest final : public TestBase
{
public:
    static constexpr std::string_view k_name = "diagnostics";
    std::string_view name() const noexcept override
    {
        return k_name;
    }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        spdlog::logger* const loggerPtr = logger_.get();
        std::atomic<int64_t>  iteration{ 0 };

        // ---- collector ----
        // Natural acquisition order: metricsMutex_ -> logBufferMutex_
        // Updates the metrics map, then appends a formatted snapshot to logBuffer_.
        std::thread collector([&stop, stress, &iteration, &metricsMutex=metricsMutex_, &logBufferMutex=logBufferMutex_, &metrics=metrics_, &logBuffer=logBuffer_]
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    const int64_t i = iteration.fetch_add(1, std::memory_order_relaxed);

                    // Acquires both locks atomically.
                    // Natural order here is metrics -> buffer, which is the
                    // OPPOSITE of reporter's natural order � deadlock territory
                    // without scoped_lock.
                    // [[C++ 17 : std::scoped_lock]]
                    std::scoped_lock lock(metricsMutex, logBufferMutex);

                    if (auto [it, inserted] = metrics.emplace("iteration", diagnostics::MetricValue{ i }); !inserted)
                    {
                        it->second = diagnostics::MetricValue{ i };
                    }

                    metrics["rate"] = diagnostics::MetricValue{ static_cast<double>(i) / (i + 1) };
                    metrics["label"] = diagnostics::MetricValue{ std::string(stress ? "stress" : "normal") };

                    for (const auto& [key, value] : metrics)
                    {
                        logBuffer += key + "=" + diagnostics::formatMetric(value) + " ";
                    }
                    logBuffer += '\n';

                    HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 20);
                }
            });

        // ---- reporter ----
        // Natural acquisition order: logBufferMutex_ -> metricsMutex_
        // Drains logBuffer_ first, then reads metrics_ for the count � inverted
        // relative to collector. This is what makes the two-mutex scoped_lock
        // load-bearing rather than decorative.
        std::thread reporter;
        try
        {
            reporter = std::thread([&stop, stress, loggerPtr, &metricsMutex=metricsMutex_, &logBufferMutex=logBufferMutex_, &metrics=metrics_, &logBuffer=logBuffer_]
                {
                    while (!stop.load(std::memory_order_acquire))
                    {
                        std::string drainedBuffer;
                        int64_t     metricCount = 0;

                        {
                            // Note the listing order: log_buffer first, then metrics.
                            // Opposite of collector. std::scoped_lock resolves this safely.
                            std::scoped_lock lock(logBufferMutex, metricsMutex);
                            drainedBuffer.swap(logBuffer);
                            metricCount = static_cast<int64_t>(metrics.size());
                        }

                        if (!drainedBuffer.empty())
                        {
                            loggerPtr->info("[reporter] {} metric(s): {}", metricCount, drainedBuffer);
                        }

                        HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 100);
                    }
                });
        }
        catch (...)
        {
            // reporter failed to start � join collector before propagating
            collector.join();
            throw;
        }

        collector.join();
        reporter.join();

        // Post-run summary via std::any � caller can inspect without a virtual method
        const int64_t finalIterations = iteration.load(std::memory_order_relaxed);

        // [[C++ 17 : std::any]]
        std::any diagnosticTag = diagnostics::ReportSummary{
            finalIterations,
            static_cast<int64_t>(metrics_.size())
        };

        if (const auto* s = std::any_cast<diagnostics::ReportSummary>(&diagnosticTag))
        {
            loggerPtr->info("[{}] post-run: iters={} metrics={}", name().data(), s->iterations, s->metricCount); // name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data()
        }
    }

private:
    std::mutex metricsMutex_;
    std::mutex logBufferMutex_;

    std::unordered_map<std::string, diagnostics::MetricValue> metrics_;
    std::string logBuffer_;        // drained by reporter, filled by collector
};
