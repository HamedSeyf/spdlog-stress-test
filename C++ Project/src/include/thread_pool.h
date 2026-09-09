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
	explicit ThreadPool(std::optional<int> maxThreads)
		: maxThreads_(maxThreads)
	{
		if (!maxThreads_)
		{
			return;
		}

		workerThreads_.reserve(*maxThreads_);
		for (int index = 0; index < *maxThreads_; ++index)
		{
			workerThreads_.emplace_back([this]()
				{
					workerLoop();
				});
		}
	}

	~ThreadPool()
	{
		wait();

		if (!maxThreads_)
		{
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}

		workCondition_.notify_all();

		for (std::thread& worker : workerThreads_)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
	}

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	void submit(std::function<void()> task)
	{
		if (!maxThreads_)
		{
			workerThreads_.emplace_back(std::move(task));
			return;
		}

		{
			std::lock_guard<std::mutex> lock(mutex_);

			const int taskId = nextTaskId_++;
			queuedTasks_.push(QueuedTask{ taskId, std::move(task) });
		}

		workCondition_.notify_one();
	}

	void wait()
	{
		if (!maxThreads_)
		{
			for (std::thread& thread : workerThreads_)
			{
				if (thread.joinable())
				{
					thread.join();
				}
			}

			workerThreads_.clear();
			return;
		}

		std::unique_lock<std::mutex> lock(mutex_);

		doneCondition_.wait(lock, [this]()
			{
				return queuedTasks_.empty() && inProgressTasks_.empty();
			});

		if (firstException_)
		{
			std::exception_ptr exception = firstException_;
			firstException_ = nullptr;
			std::rethrow_exception(exception);
		}
	}

private:
	struct QueuedTask
	{
		int id = 0;
		std::function<void()> function;
	};

	void workerLoop()
	{
		while (true)
		{
			QueuedTask task;

			{
				std::unique_lock<std::mutex> lock(mutex_);

				workCondition_.wait(lock, [this]()
					{
						return stopping_ || !queuedTasks_.empty();
					});

				if (stopping_ && queuedTasks_.empty())
				{
					return;
				}

				task = std::move(queuedTasks_.front());
				queuedTasks_.pop();

				inProgressTasks_.insert(task.id);
			}

			try
			{
				task.function();
			}
			catch (...)
			{
				std::lock_guard<std::mutex> lock(mutex_);

				if (!firstException_)
				{
					firstException_ = std::current_exception();
				}
			}

			{
				std::lock_guard<std::mutex> lock(mutex_);

				inProgressTasks_.erase(task.id);

				if (queuedTasks_.empty() && inProgressTasks_.empty())
				{
					doneCondition_.notify_all();
				}
			}
		}
	}

	std::optional<int> maxThreads_;

	std::vector<std::thread> workerThreads_;

	std::mutex mutex_;
	std::condition_variable workCondition_;
	std::condition_variable doneCondition_;

	std::queue<QueuedTask> queuedTasks_;
	std::set<int> inProgressTasks_;

	int nextTaskId_ = 0;
	bool stopping_ = false;

	std::exception_ptr firstException_;
};
