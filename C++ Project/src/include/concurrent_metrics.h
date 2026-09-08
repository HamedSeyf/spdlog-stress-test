#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>
#include <array>
#include <optional>
#include <random>
#include <string>
#include <cstdio>

#include <spdlog/spdlog.h>

#include "hamed_common/platform.h"

#include "test_base.h"


namespace concurrent_metrics
{
    struct Event
    {
        unsigned processed_count = 0;
        unsigned failed_count = 0;
        double average_latency_us = 0;
        // Preferred over std::string to avoid heap/copy
        std::array<char, 128> last_error_message {};

        static Event CreateRandom()
        {
            thread_local std::mt19937 generator(std::random_device{}());

            std::uniform_int_distribution<unsigned> processed_distribution(50, 500);
            std::uniform_int_distribution<unsigned> failed_distribution(0, 10);
            std::uniform_real_distribution<double> latency_distribution(100.0, 5000.0);

            std::bernoulli_distribution error_distribution(0.2);
            std::uniform_int_distribution<unsigned> error_id_distribution(1000, 9999);

            Event result;

            result.processed_count = processed_distribution(generator);
            result.failed_count = failed_distribution(generator);
            result.average_latency_us = latency_distribution(generator);

            if (error_distribution(generator))
            {
                std::snprintf(
                    result.last_error_message.data(),
                    result.last_error_message.size(),
                    "Random worker processing failure #%u",
                    error_id_distribution(generator));
            }

            return result;
        }
        static std::optional<Event> Consolidate(const std::vector<Event>& events)
        {
            if (events.empty())
            {
                return std::nullopt;
            }

            std::optional<Event> result(std::in_place);

            for (const Event& current_event : events)
            {
                result->EnvelopeWith(current_event);
            }

            return result;
        }
        void EnvelopeWith(const Event& event)
        {
            const unsigned previous_processed_count = processed_count;
            const unsigned incoming_processed_count = event.processed_count;
            const unsigned combined_processed_count = previous_processed_count + incoming_processed_count;

            if (combined_processed_count > 0)
            {
                average_latency_us = 
                    ((average_latency_us * previous_processed_count) + (event.average_latency_us * incoming_processed_count)) / combined_processed_count;
            }

            processed_count += event.processed_count;
            failed_count += event.failed_count;

            if (event.last_error_message[0] != '\0')
            {
                last_error_message = event.last_error_message;
            }
        }
    };
}


// ============================================================
// ConcurrentMetricsTest
// ============================================================
class ConcurrentMetricsTest final : public TestBase
{
public:
    static constexpr std::string_view k_name = "concurrent_metrics_test";
    std::string_view name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        internal_stop_.store(true, std::memory_order_release);
        Wait();
        internal_stop_.store(false, std::memory_order_release);
        worker_threads_.clear();
        shared_data_.reset();

        // Extract raw observer pointer once � stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const logger_ptr = logger_.get();

        constexpr unsigned workers_count = 3; // Whats the best naming convention for such consts and is this the best spot to put these file scoped constexpr?
        constexpr unsigned event_batch_size = 64;
        constexpr unsigned reporter_nonstress_sleep_duration_ms = 100;

        worker_threads_.reserve(workers_count);

        // First creating worker threads
        for (unsigned worker_id = 0; worker_id < workers_count; ++worker_id)
        {
            try
            {
                worker_threads_.emplace_back([&stop, stress, name = name(), & internal_stop = internal_stop_, &shared_data_mutex = shared_data_mutex_, &shared_data = shared_data_]()
                    {
                        try
                        {
                            std::vector<concurrent_metrics::Event> unreported_events;
                            unreported_events.reserve(event_batch_size);

                            auto flush_lambda = [&unreported_events, &shared_data_mutex, &shared_data]()
                                {
                                    std::optional<concurrent_metrics::Event> consolidated_event = concurrent_metrics::Event::Consolidate(unreported_events);
                                    unreported_events.clear();

                                    // Dumping all the batched events data into the common data pool
                                    if (consolidated_event.has_value())
                                    {
                                        std::lock_guard<std::mutex> data_lock(shared_data_mutex);
                                        if (!shared_data.has_value())
                                        {
                                            shared_data.emplace(*consolidated_event);
                                        }
                                        else
                                        {
                                            shared_data->EnvelopeWith(*consolidated_event);
                                        }
                                    }
                                };

                            while (!stop.load(std::memory_order_acquire) && !internal_stop.load(std::memory_order_acquire))
                            {
                                // Randomly creating and saving an event
                                unreported_events.emplace_back(concurrent_metrics::Event::CreateRandom());

                                if (unreported_events.size() >= event_batch_size)
                                {
                                    flush_lambda();
                                }

                                HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 100);
                            }

                            flush_lambda();
                        }
                        catch (const std::exception& e)
                        {
                            fprintf(stderr, "[fatal] test '%s' threw: %s\n", name.data(), e.what());
                            internal_stop.store(true, std::memory_order_release);
                        }
                        catch (...)
                        {
                            fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name.data());
                            internal_stop.store(true, std::memory_order_release);
                        }
                    });
            }
            catch (const std::exception& e)
            {
                fprintf(stderr, "[fatal] test '%s' threw: %s\n", name().data(), e.what());
                internal_stop_.store(true, std::memory_order_release);
                break;
            }
            catch (...)
            {
                fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name().data());
                internal_stop_.store(true, std::memory_order_release);
                break;
            }
        }

        // And now creating the reporter thread
        try
        {
            reporter_thread_ = std::thread([&stop, stress, name = name(), &internal_stop = internal_stop_, &shared_data_mutex = shared_data_mutex_, &shared_data = shared_data_, logger_ptr]()
                {
                    try
                    {
                        auto report_lambda = [&shared_data, &shared_data_mutex, logger_ptr]()
                            {
                                std::optional<concurrent_metrics::Event> shared_data_copy;

                                {
                                    std::lock_guard<std::mutex> data_lock(shared_data_mutex);
                                    shared_data_copy = shared_data;
                                }

                                if (shared_data_copy.has_value())
                                {
                                    logger_ptr->info("processed_count={} failed_count={} average_latency_us={} last_error_message={}",
                                        shared_data_copy->processed_count,
                                        shared_data_copy->failed_count,
                                        shared_data_copy->average_latency_us,
                                        shared_data_copy->last_error_message.data());
                                }
                                else
                                {
                                    logger_ptr->info("No metrics produced yet.");
                                }
                            };

                        while (!stop.load(std::memory_order_acquire) && !internal_stop.load(std::memory_order_acquire))
                        {
                            report_lambda();

                            HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, reporter_nonstress_sleep_duration_ms);
                        }

                        report_lambda();
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw: %s\n", name.data(), e.what());
                        internal_stop.store(true, std::memory_order_release);
                    }
                    catch (...)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name.data());
                        internal_stop.store(true, std::memory_order_release);
                    }
                });
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[fatal] test '%s' threw: %s\n", name().data(), e.what());
            internal_stop_.store(true, std::memory_order_release);
        }
        catch (...)
        {
            fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name().data());
            internal_stop_.store(true, std::memory_order_release);
        }

        Wait();

        if (internal_stop_.load(std::memory_order_acquire))
        {
            throw std::runtime_error("[fatal] Early exiting due to internal exception(s).");
        }
    }

private:
    std::vector<std::thread> worker_threads_;
    std::thread reporter_thread_;

    std::mutex shared_data_mutex_;
    std::optional<concurrent_metrics::Event> shared_data_;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignas — expected for cache-line alignment
#endif
    HAMEDSEYF_CACHE_ALIGN std::atomic<bool> internal_stop_ { false };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    void Wait()
    {
        std::vector<std::thread> local_threads;

        local_threads.swap(worker_threads_);
        local_threads.push_back(std::move(reporter_thread_));

        for (std::thread& current_thread : local_threads)
        {
            if (current_thread.joinable())
            {
                current_thread.join();
            }
        }
    }
};
