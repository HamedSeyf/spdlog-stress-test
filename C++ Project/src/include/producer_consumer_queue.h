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
        unsigned ProducerId = 0;
        unsigned SequenceId = 0;
        std::chrono::steady_clock::time_point CreatedAt;
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
        // Extract raw observer pointer once — stays in register for entire loop - lifetime guaranteed: logger_ outlives run() by design
        spdlog::logger* const logger_ptr = logger_.get();

        constexpr unsigned producer_count = 2;
        constexpr unsigned consumer_count = 2;
        constexpr unsigned max_queue_size = 2000;

        // First creating producer threads
        for (unsigned producer_id = 0; producer_id < producer_count; ++producer_id)
        {
            threads_.emplace_back([&stop, stress, &work_items_mutex = work_items_mutex_, &work_items = work_items_, &work_items_cv = work_items_cv_, &latest_sequence_id = latest_sequence_id_, producer_id]()
                {
                    while (!stop.load(std::memory_order_acquire))
                    {
                        bool should_notify = false;

                        {
                            std::lock_guard<std::mutex> lock(work_items_mutex);

                            if (work_items.size() < max_queue_size)
                            {
                                work_items.push(producer_consumer_queue::WorkItem{ producer_id, latest_sequence_id++, std::chrono::steady_clock::now() });
                                should_notify = true;
                            }
                        }

                        if (should_notify)
                        {
                            work_items_cv.notify_one();
                        }

                        HAMEDSEYF_SPIN_OR_SLEEP_MS(stress, 50);
                    }
                });
        }

        // And now creating consumer threads
        for (unsigned consumer_id = 0; consumer_id < consumer_count; ++consumer_id)
        {
            threads_.emplace_back([&stop, stress, &work_items_mutex = work_items_mutex_, &work_items = work_items_, &work_items_cv = work_items_cv_, consumer_id, logger_ptr]()
                {
                    std::unique_lock<std::mutex> lock(work_items_mutex);

                    while (!stop.load(std::memory_order_acquire))
                    {
                        work_items_cv.wait_for(lock, std::chrono::milliseconds(10), [&work_items, &stop]
                            {
                                return stop.load(std::memory_order_acquire) || !work_items.empty();
                            });

                        if (work_items.empty())
                        {
                            if (stop.load(std::memory_order_acquire))
                            {
                                return; // stop has been externally set to true so need to exit
                            }

                            continue;
                        }

                        producer_consumer_queue::WorkItem popped_item(std::move(work_items.front()));
                        work_items.pop();

                        const size_t work_items_size = work_items.size();

                        lock.unlock();

                        const long long latency_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - popped_item.CreatedAt).count();

                        logger_ptr->info("consumer={} producer={} seq={} latency_us={} queue_size={}", consumer_id, popped_item.ProducerId, popped_item.SequenceId, latency_us, work_items_size);

                        lock.lock();
                    }
                });
        }

        Wait();
    }

private:
    std::mutex work_items_mutex_;
    std::queue<producer_consumer_queue::WorkItem> work_items_;
    std::condition_variable work_items_cv_;
    unsigned latest_sequence_id_ = 0;

    std::vector<std::thread> threads_;

    void Wait()
    {
        std::vector<std::thread> local_threads;

        local_threads.swap(threads_);

        for (std::thread& current_thread : local_threads)
        {
            if (current_thread.joinable())
            {
                current_thread.join();
            }
        }
    }
};
