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
    inline std::string format_metric(const MetricValue& v)
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
        int64_t metric_count = 0;
    };
} // namespace diagnostics


// ============================================================
// DiagnosticsTest
// ============================================================
// Two internal threads share metrics_mutex_ and log_buffer_mutex_:
//
//   collector � natural order: metrics_ first, then log_buffer_
//   reporter  � natural order: log_buffer_ first, then metrics_
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
        spdlog::logger* const logger_ptr = logger_.get();
        std::atomic<int64_t>  iteration{ 0 };

        // ---- collector ----
        // Natural acquisition order: metrics_mutex_ -> log_buffer_mutex_
        // Updates the metrics map, then appends a formatted snapshot to log_buffer_.
        std::thread collector([&stop, stress, &iteration, &metrics_mutex=metrics_mutex_, &log_buffer_mutex=log_buffer_mutex_, &metrics=metrics_, &log_buffer=log_buffer_]
            {
                while (!stop.load(std::memory_order_acquire))
                {
                    const int64_t i = iteration.fetch_add(1, std::memory_order_relaxed);

                    // Acquires both locks atomically.
                    // Natural order here is metrics -> buffer, which is the
                    // OPPOSITE of reporter's natural order � deadlock territory
                    // without scoped_lock.
                    // [[C++ 17 : std::scoped_lock]]
                    std::scoped_lock lock(metrics_mutex, log_buffer_mutex);

                    if (auto [it, inserted] = metrics.emplace("iteration", diagnostics::MetricValue{ i }); !inserted)
                    {
                        it->second = diagnostics::MetricValue{ i };
                    }

                    metrics["rate"] = diagnostics::MetricValue{ static_cast<double>(i) / (i + 1) };
                    metrics["label"] = diagnostics::MetricValue{ std::string(stress ? "stress" : "normal") };

                    for (const auto& [key, value] : metrics)
                    {
                        log_buffer += key + "=" + diagnostics::format_metric(value) + " ";
                    }
                    log_buffer += '\n';

                    HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 20);
                }
            });

        // ---- reporter ----
        // Natural acquisition order: log_buffer_mutex_ -> metrics_mutex_
        // Drains log_buffer_ first, then reads metrics_ for the count � inverted
        // relative to collector. This is what makes the two-mutex scoped_lock
        // load-bearing rather than decorative.
        std::thread reporter;
        try
        {
            reporter = std::thread([&stop, stress, logger_ptr, &metrics_mutex=metrics_mutex_, &log_buffer_mutex=log_buffer_mutex_, &metrics=metrics_, &log_buffer=log_buffer_]
                {
                    while (!stop.load(std::memory_order_acquire))
                    {
                        std::string drained_buffer;
                        int64_t     metric_count = 0;

                        {
                            // Note the listing order: log_buffer first, then metrics.
                            // Opposite of collector. std::scoped_lock resolves this safely.
                            std::scoped_lock lock(log_buffer_mutex, metrics_mutex);
                            drained_buffer.swap(log_buffer);
                            metric_count = static_cast<int64_t>(metrics.size());
                        }

                        if (!drained_buffer.empty())
                        {
                            logger_ptr->info("[reporter] {} metric(s): {}", metric_count, drained_buffer);
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
        const int64_t final_iterations = iteration.load(std::memory_order_relaxed);

        // [[C++ 17 : std::any]]
        std::any diagnostic_tag = diagnostics::ReportSummary{
            final_iterations,
            static_cast<int64_t>(metrics_.size())
        };

        if (const auto* s = std::any_cast<diagnostics::ReportSummary>(&diagnostic_tag))
        {
            logger_ptr->info("[{}] post-run: iters={} metrics={}", name().data(), s->iterations, s->metric_count); // name() returns a string_view literal and always guaranteed to be null terminated so it's safe to use data()
        }
    }

private:
    std::mutex metrics_mutex_;
    std::mutex log_buffer_mutex_;

    std::unordered_map<std::string, diagnostics::MetricValue> metrics_;
    std::string log_buffer_;        // drained by reporter, filled by collector
};
