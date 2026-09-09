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
        unsigned processedCount = 0;
        unsigned failedCount = 0;
        double averageLatencyUs = 0;
        // Preferred over std::string to avoid heap/copy
        std::array<char, 128> lastErrorMessage {};

        static Event createRandom()
        {
            thread_local std::mt19937 generator(std::random_device{}());

            std::uniform_int_distribution<unsigned> processedDistribution(50, 500);
            std::uniform_int_distribution<unsigned> failedDistribution(0, 10);
            std::uniform_real_distribution<double> latencyDistribution(100.0, 5000.0);

            std::bernoulli_distribution errorDistribution(0.2);
            std::uniform_int_distribution<unsigned> errorIdDistribution(1000, 9999);

            Event result;

            result.processedCount = processedDistribution(generator);
            result.failedCount = failedDistribution(generator);
            result.averageLatencyUs = latencyDistribution(generator);

            if (errorDistribution(generator))
            {
                std::snprintf(
                    result.lastErrorMessage.data(),
                    result.lastErrorMessage.size(),
                    "Random worker processing failure #%u",
                    errorIdDistribution(generator));
            }

            return result;
        }
        static std::optional<Event> consolidate(const std::vector<Event>& events)
        {
            if (events.empty())
            {
                return std::nullopt;
            }

            std::optional<Event> result(std::in_place);

            for (const Event& currentEvent : events)
            {
                result->envelopeWith(currentEvent);
            }

            return result;
        }
        void envelopeWith(const Event& event)
        {
            const unsigned previousProcessedCount = processedCount;
            const unsigned incomingProcessedCount = event.processedCount;
            const unsigned combinedProcessedCount = previousProcessedCount + incomingProcessedCount;

            if (combinedProcessedCount > 0)
            {
                averageLatencyUs =
                    ((averageLatencyUs * previousProcessedCount) + (event.averageLatencyUs * incomingProcessedCount)) / combinedProcessedCount;
            }

            processedCount += event.processedCount;
            failedCount += event.failedCount;

            if (event.lastErrorMessage[0] != '\0')
            {
                lastErrorMessage = event.lastErrorMessage;
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
        internalStop_.store(true, std::memory_order_release);
        wait();
        internalStop_.store(false, std::memory_order_release);
        workerThreads_.clear();
        sharedData_.reset();

        // Extract raw observer pointer once � stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const loggerPtr = logger_.get();

        constexpr unsigned workersCount = 3; // Whats the best naming convention for such consts and is this the best spot to put these file scoped constexpr?
        constexpr unsigned eventBatchSize = 64;
        constexpr unsigned reporterNonstressSleepDurationMs = 100;

        workerThreads_.reserve(workersCount);

        // First creating worker threads
        for (unsigned workerId = 0; workerId < workersCount; ++workerId)
        {
            try
            {
                workerThreads_.emplace_back([&stop, stress, name = name(), & internalStop = internalStop_, &sharedDataMutex = sharedDataMutex_, &sharedData = sharedData_]()
                    {
                        try
                        {
                            std::vector<concurrent_metrics::Event> unreportedEvents;
                            unreportedEvents.reserve(eventBatchSize);

                            auto flushLambda = [&unreportedEvents, &sharedDataMutex, &sharedData]()
                                {
                                    std::optional<concurrent_metrics::Event> consolidatedEvent = concurrent_metrics::Event::consolidate(unreportedEvents);
                                    unreportedEvents.clear();

                                    // Dumping all the batched events data into the common data pool
                                    if (consolidatedEvent.has_value())
                                    {
                                        std::lock_guard<std::mutex> dataLock(sharedDataMutex);
                                        if (!sharedData.has_value())
                                        {
                                            sharedData.emplace(*consolidatedEvent);
                                        }
                                        else
                                        {
                                            sharedData->envelopeWith(*consolidatedEvent);
                                        }
                                    }
                                };

                            while (!stop.load(std::memory_order_acquire) && !internalStop.load(std::memory_order_acquire))
                            {
                                // Randomly creating and saving an event
                                unreportedEvents.emplace_back(concurrent_metrics::Event::createRandom());

                                if (unreportedEvents.size() >= eventBatchSize)
                                {
                                    flushLambda();
                                }

                                HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 100);
                            }

                            flushLambda();
                        }
                        catch (const std::exception& e)
                        {
                            fprintf(stderr, "[fatal] test '%s' threw: %s\n", name.data(), e.what());
                            internalStop.store(true, std::memory_order_release);
                        }
                        catch (...)
                        {
                            fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name.data());
                            internalStop.store(true, std::memory_order_release);
                        }
                    });
            }
            catch (const std::exception& e)
            {
                fprintf(stderr, "[fatal] test '%s' threw: %s\n", name().data(), e.what());
                internalStop_.store(true, std::memory_order_release);
                break;
            }
            catch (...)
            {
                fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name().data());
                internalStop_.store(true, std::memory_order_release);
                break;
            }
        }

        // And now creating the reporter thread
        try
        {
            reporterThread_ = std::thread([&stop, stress, name = name(), &internalStop = internalStop_, &sharedDataMutex = sharedDataMutex_, &sharedData = sharedData_, loggerPtr]()
                {
                    try
                    {
                        auto reportLambda = [&sharedData, &sharedDataMutex, loggerPtr]()
                            {
                                std::optional<concurrent_metrics::Event> sharedDataCopy;

                                {
                                    std::lock_guard<std::mutex> dataLock(sharedDataMutex);
                                    sharedDataCopy = sharedData;
                                }

                                if (sharedDataCopy.has_value())
                                {
                                    loggerPtr->info("processed_count={} failed_count={} average_latency_us={} last_error_message={}",
                                        sharedDataCopy->processedCount,
                                        sharedDataCopy->failedCount,
                                        sharedDataCopy->averageLatencyUs,
                                        sharedDataCopy->lastErrorMessage.data());
                                }
                                else
                                {
                                    loggerPtr->info("No metrics produced yet.");
                                }
                            };

                        while (!stop.load(std::memory_order_acquire) && !internalStop.load(std::memory_order_acquire))
                        {
                            reportLambda();

                            HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, reporterNonstressSleepDurationMs);
                        }

                        reportLambda();
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw: %s\n", name.data(), e.what());
                        internalStop.store(true, std::memory_order_release);
                    }
                    catch (...)
                    {
                        fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name.data());
                        internalStop.store(true, std::memory_order_release);
                    }
                });
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[fatal] test '%s' threw: %s\n", name().data(), e.what());
            internalStop_.store(true, std::memory_order_release);
        }
        catch (...)
        {
            fprintf(stderr, "[fatal] test '%s' threw unknown exception\n", name().data());
            internalStop_.store(true, std::memory_order_release);
        }

        wait();

        if (internalStop_.load(std::memory_order_acquire))
        {
            throw std::runtime_error("[fatal] Early exiting due to internal exception(s).");
        }
    }

private:
    std::vector<std::thread> workerThreads_;
    std::thread reporterThread_;

    std::mutex sharedDataMutex_;
    std::optional<concurrent_metrics::Event> sharedData_;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // structure padded due to alignas — expected for cache-line alignment
#endif
    HAMEDSEYF_CACHE_ALIGN std::atomic<bool> internalStop_ { false };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    void wait()
    {
        std::vector<std::thread> localThreads;

        localThreads.swap(workerThreads_);
        localThreads.push_back(std::move(reporterThread_));

        for (std::thread& currentThread : localThreads)
        {
            if (currentThread.joinable())
            {
                currentThread.join();
            }
        }
    }
};
