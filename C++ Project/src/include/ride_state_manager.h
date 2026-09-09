#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <queue>
#include <vector>
#include <array>
#include <set>
#include <optional>
#include <random>
#include <string>
#include <cstdio>


// ============================================================
// RideState
// ============================================================
enum class RideState
{
    Idle,
    Requested,
    PassengerBoarding,
    InProgress,
    Completed,
};


// ============================================================
// NotificationData
// ============================================================
struct NotificationData
{
	RideState previousState;
	RideState currentState;
};


// ============================================================
// IRideStateObserver
// ============================================================
class IRideStateObserver
{
public:
	virtual ~IRideStateObserver() = default;

	virtual void onRideStateChanged(RideState oldState, RideState newState) = 0;
};


// ============================================================
// IObserverDispatcher
// ============================================================
class IObserverDispatcher
{
public:
	virtual ~IObserverDispatcher() = default;

	virtual void dispatch(std::shared_ptr<IRideStateObserver> observer, const NotificationData& data) = 0;
};


// ============================================================
// ObserverData
// ============================================================
struct ObserverData
{
	std::weak_ptr<IObserverDispatcher> dispatcher;
	std::weak_ptr<IRideStateObserver> observer;
};


// ============================================================
// RideStateManager
// ============================================================
class RideStateManager : public TestBase
{
public:
    static constexpr std::string_view k_name = "ride_state_manager";
    std::string_view name() const noexcept override { return k_name; }

public:

	virtual void run(const std::atomic<bool>& stop, [[maybe_unused]] bool stress) override
	{
		observerThread_ = std::thread([this, &stop]()
			{
				NotificationData frontNotificationData;

				while (!stop.load(std::memory_order_acquire))
				{
					std::unique_lock<std::mutex> notificationsLock(notificationsMutex_);

					notificationQueueCv_.wait(notificationsLock,[&isShuttingDown = isShuttingDown_, &notificationsQueue = notificationsQueue_]()
						{
							return (isShuttingDown || !notificationsQueue.empty());
						});

					if (isShuttingDown_ && notificationsQueue_.empty())
					{
						return;
					}

					if (notificationsQueue_.empty())
					{
						continue;
					}

					frontNotificationData = std::move(notificationsQueue_.front());
					notificationsQueue_.pop();

					notificationsLock.unlock();

					using HealthyObserversPairType = std::pair<std::shared_ptr<IObserverDispatcher>, std::shared_ptr<IRideStateObserver>>;

					std::vector<HealthyObserversPairType> healthyObservers;

					{
						// In this scope, the expired weak pointers are cleaned up and a copy style vector of the healthy shared_ptr gets created for the callback (next step)
						std::lock_guard<std::mutex> observersLock(observersMutex_);

						healthyObservers.reserve(observers_.size());

						for (auto iter = observers_.begin(); iter != observers_.end();)
						{
							std::shared_ptr<IObserverDispatcher> dispatcher = iter->dispatcher.lock();
							std::shared_ptr<IRideStateObserver> observer = iter->observer.lock();

							if (dispatcher && observer)
							{
								healthyObservers.push_back({ dispatcher, observer });
								++iter;
							}
							else
							{
								iter = observers_.erase(iter);
							}
						}
					}

					for (const HealthyObserversPairType& currentObserverData : healthyObservers)
					{
						try
						{
							currentObserverData.first->dispatch(currentObserverData.second, frontNotificationData);
						}
						catch (const std::exception& exception)
						{
							std::fprintf(stderr, "Observer callback threw exception : %s\n", exception.what());
						}
						catch (...)
						{
							std::fprintf(stderr, "Observer callback threw unknown exception\n");
						}
					}
				}
			});
	}

	virtual ~RideStateManager()
	{
		{
			std::lock_guard<std::mutex> lock(notificationsMutex_);
			isShuttingDown_ = true;
		}

		notificationQueueCv_.notify_one();

		if (observerThread_.joinable())
		{
			observerThread_.join();
		}
	}

	bool subscribe(const std::shared_ptr<IObserverDispatcher>& dispatcher, const std::shared_ptr<IRideStateObserver>&  observer)
	{
		if (!dispatcher || !observer)
		{
			return false;
		}

		{
			std::scoped_lock lock(observersMutex_, notificationsMutex_);

			if (isShuttingDown_)
			{
				return false;
			}

			for (auto iter = observers_.begin(); iter != observers_.end();)
			{
				std::shared_ptr<IObserverDispatcher> currentDispatcher = iter->dispatcher.lock();
				std::shared_ptr<IRideStateObserver> currentObserver = iter->observer.lock();

				if (currentDispatcher && currentObserver)
				{
					if (currentDispatcher == dispatcher && currentObserver == observer)
					{
						return false;
					}
					else
					{
						++iter;
					}
				}
				else
				{
					iter = observers_.erase(iter);
				}
			}

			observers_.push_back({ dispatcher, observer });

			return true;
		}
	}

	bool unsubscribe(const std::shared_ptr<IRideStateObserver>& observer)
	{
		if (!observer)
		{
			return false;
		}

		{
			std::scoped_lock lock(observersMutex_, notificationsMutex_);

			if (isShuttingDown_)
			{
				return false;
			}

			for (auto iter = observers_.begin(); iter != observers_.end();)
			{
				if (auto currentObserver = iter->observer.lock())
				{
					if (currentObserver == observer)
					{
						iter = observers_.erase(iter);
					}
					else
					{
						++iter;
					}
				}
				else
				{
					iter = observers_.erase(iter);
				}
			}
		}

		return true;
	}

	bool resetToIdle() { return tryChangingState(RideState::Idle); }
    bool requestRide() { return tryChangingState(RideState::Requested); }
    bool startBoarding() { return tryChangingState(RideState::PassengerBoarding); }
    bool startRide() { return tryChangingState(RideState::InProgress); }
    bool completeRide() { return tryChangingState(RideState::Completed); }

    RideState getCurrentState() const
	{
		std::lock_guard<std::mutex> lock(rideStateMutex_);
		return currentRideState_;
	}

private:
    RideState currentRideState_ = RideState::Idle;
	mutable std::mutex rideStateMutex_;

    std::vector<ObserverData> observers_;
	std::mutex observersMutex_;

	std::thread observerThread_;

	std::queue<NotificationData> notificationsQueue_;
	std::condition_variable notificationQueueCv_;
	std::mutex notificationsMutex_;
	bool isShuttingDown_ = false;

	static constexpr std::size_t k_maxPendingNotifications = 1024;

	bool tryChangingState(const RideState nextRideState)
	{
		RideState oldRideState;

		{
			std::scoped_lock lock(rideStateMutex_, notificationsMutex_);

			if (!isValidTransition(currentRideState_, nextRideState) || (notificationsQueue_.size() >= k_maxPendingNotifications) || isShuttingDown_)
			{
				return false;
			}

			oldRideState = currentRideState_;
			currentRideState_ = nextRideState;

			notificationsQueue_.push({ oldRideState, nextRideState });

			notificationQueueCv_.notify_one();
		}

		return true;
	}

	static bool isValidTransition(RideState current, RideState next)
	{
		switch (next)
		{
		case RideState::Idle:
		{
			if (current == RideState::Idle) return false;
			break;
		}
		case RideState::Requested:
		{
			if (current != RideState::Idle) return false;
			break;
		}
		case RideState::PassengerBoarding:
		{
			if (current != RideState::Requested) return false;
			break;
		}
		case RideState::InProgress:
		{
			if (current != RideState::PassengerBoarding) return false;
			break;
		}
		case RideState::Completed:
		{
			if (current != RideState::InProgress) return false;
			break;
		}
		}

		return true;
	}
};
