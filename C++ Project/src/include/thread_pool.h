#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <thread>
#include <vector>


class ThreadPool
{
public:
	explicit ThreadPool(std::optional<int> max_threads)
		: max_threads_(max_threads)
	{
		if (!max_threads_)
		{
			return;
		}

		workers_threads_.reserve(*max_threads_);
		for (int index = 0; index < *max_threads_; ++index)
		{
			workers_threads_.emplace_back([this]()
				{
					WorkerLoop();
				});
		}
	}

	~ThreadPool()
	{
		Wait();

		if (!max_threads_)
		{
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}

		work_condition_.notify_all();

		for (std::thread& worker : workers_threads_)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	void Submit(std::function<void()> task)
	{
		if (!max_threads_)
		{
			workers_threads_.emplace_back(std::move(task));
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);

			const int task_id = next_task_id_++;
			queued_tasks_.push(QueuedTask{ task_id, std::move(task) });
		}

		work_condition_.notify_one();
	}

	void Wait()
	{
		if (!max_threads_)
		{
			for (std::thread& thread : workers_threads_)
			{
				if (thread.joinable())
				{
					thread.join();
				}
			}

			workers_threads_.clear();
			return;
		}

		std::unique_lock<std::mutex> lock(mutex_);

		done_condition_.wait(lock, [this]()
			{
				return queued_tasks_.empty() && in_progress_tasks_.empty();
			});

		if (first_exception_)
		{
			std::exception_ptr exception = first_exception_;
			first_exception_ = nullptr;
			std::rethrow_exception(exception);
		}
	}

private:
	struct QueuedTask
	{
		int id = 0;
		std::function<void()> function;
	};

	void WorkerLoop()
	{
		while (true)
		{
			QueuedTask task;

			{
				std::unique_lock<std::mutex> lock(mutex_);

				work_condition_.wait(lock, [this]()
					{
						return stopping_ || !queued_tasks_.empty();
					});

				if (stopping_ && queued_tasks_.empty())
				{
					return;
				}

				task = std::move(queued_tasks_.front());
				queued_tasks_.pop();

				in_progress_tasks_.insert(task.id);
			}

			try
			{
				task.function();
			}
			catch (...)
			{
				std::lock_guard<std::mutex> lock(mutex_);

				if (!first_exception_)
				{
					first_exception_ = std::current_exception();
				}
			}

			{
				std::lock_guard<std::mutex> lock(mutex_);

				in_progress_tasks_.erase(task.id);

				if (queued_tasks_.empty() && in_progress_tasks_.empty())
				{
					done_condition_.notify_all();
				}
			}
		}
	}

	std::optional<int> max_threads_;

	std::vector<std::thread> workers_threads_;

	std::mutex mutex_;
	std::condition_variable work_condition_;
	std::condition_variable done_condition_;

	std::queue<QueuedTask> queued_tasks_;
	std::set<int> in_progress_tasks_;

	int next_task_id_ = 0;
	bool stopping_ = false;

	std::exception_ptr first_exception_;
};
