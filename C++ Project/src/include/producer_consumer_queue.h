#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

#include <spdlog/spdlog.h>

#include "test_base.h"


namespace producer_consumer_queue
{
    struct WorkItem
    {
        unsigned producerId = 0;
        unsigned sequenceId = 0;
        std::chrono::steady_clock::time_point createdAt;
    };
}


// ============================================================
// ProducerConsumerQueueTest
// ============================================================
class ProducerConsumerQueueTest final : public TestBase
{
public:
    static constexpr std::string_view k_name = "producer_consumer_queue";
    std::string_view name() const noexcept override { return k_name; }

protected:
    void run(const std::atomic<bool>& stop, bool stress) override
    {
        // Extract raw observer pointer once � stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const loggerPtr = logger_.get();

        constexpr unsigned producerCount = 2;
        constexpr unsigned consumerCount = 2;
        constexpr unsigned maxQueueSize = 2000;

        // First creating producer threads
        for (unsigned producerId = 0; producerId < producerCount; ++producerId)
        {
            threads_.emplace_back([&stop, stress, &workItemsMutex = workItemsMutex_, &workItems = workItems_, &workItemsCv = workItemsCv_, &latestSequenceId = latestSequenceId_, producerId]()
                {
                    while (!stop.load(std::memory_order_acquire))
                    {
                        bool shouldNotify = false;

                        {
                            std::lock_guard<std::mutex> lock(workItemsMutex);

                            if (workItems.size() < maxQueueSize)
                            {
                                workItems.push(producer_consumer_queue::WorkItem{ producerId, latestSequenceId++, std::chrono::steady_clock::now() });
                                shouldNotify = true;
                            }
                        }

                        if (shouldNotify)
                        {
                            workItemsCv.notify_one();
                        }

                        HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 50);
                    }
                });
        }

        // And now creating consumer threads
        for (unsigned consumerId = 0; consumerId < consumerCount; ++consumerId)
        {
            threads_.emplace_back([&stop, stress, &workItemsMutex = workItemsMutex_, &workItems = workItems_, &workItemsCv = workItemsCv_, consumerId, loggerPtr]()
                {
                    std::unique_lock<std::mutex> lock(workItemsMutex);

                    while (!stop.load(std::memory_order_acquire))
                    {
                        workItemsCv.wait_for(lock, std::chrono::milliseconds(10), [&workItems, &stop]
                            {
                                return stop.load(std::memory_order_acquire) || !workItems.empty();
                            });

                        if (workItems.empty())
                        {
                            if (stop.load(std::memory_order_acquire))
                            {
                                return; // stop has been externally set to true so need to exit
                            }

                            continue;
                        }

                        producer_consumer_queue::WorkItem poppedItem(std::move(workItems.front()));
                        workItems.pop();

                        const size_t workItemsSize = workItems.size();

                        lock.unlock();

                        const long long latencyUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - poppedItem.createdAt).count();

                        loggerPtr->info("consumer={} producer={} seq={} latency_us={} queue_size={}", consumerId, poppedItem.producerId, poppedItem.sequenceId, latencyUs, workItemsSize);

                        lock.lock();
                    }
                });
        }

        wait();
    }

private:
    std::mutex workItemsMutex_;
    std::queue<producer_consumer_queue::WorkItem> workItems_;
    std::condition_variable workItemsCv_;
    unsigned latestSequenceId_ = 0;

    std::vector<std::thread> threads_;

    void wait()
    {
        std::vector<std::thread> localThreads;

        localThreads.swap(threads_);

        for (std::thread& currentThread : localThreads)
        {
            if (currentThread.joinable())
            {
                currentThread.join();
            }
        }
    }
};
