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

// TODO: temp remove post testing
#include "generic_notification_manager.h"


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
	RideState PreviousState;
	RideState CurrentState;
};


// ============================================================
// IRideStateObserver
// ============================================================
class IRideStateObserver
{
public:
	virtual ~IRideStateObserver() = default;

	virtual void OnRideStateChanged(RideState OldState, RideState NewState) = 0;
};


// ============================================================
// IObserverDispatcher
// ============================================================
class IObserverDispatcher
{
public:
	virtual ~IObserverDispatcher() = default;

	virtual void Dispatch(std::shared_ptr<IRideStateObserver> Observer, const NotificationData& Data) = 0;
};


// ============================================================
// ObserverData
// ============================================================
struct ObserverData
{
	std::weak_ptr<IObserverDispatcher> Dispatcher;
	std::weak_ptr<IRideStateObserver> Observer;
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

	RideStateManager()
	{
		ObserverThread = std::thread([this]()
			{
				NotificationData FrontNotificationData;

				while (true)
				{
					std::unique_lock<std::mutex> NotificationsLock(NotificationsMutex);

					NotificationQueueCV.wait(NotificationsLock,[&bShuttingDown = bShuttingDown, &NotificationsQueue = NotificationsQueue]()
						{
							return (bShuttingDown || !NotificationsQueue.empty());
						});

					if (bShuttingDown && NotificationsQueue.empty())
					{
						return;
					}

					if (NotificationsQueue.empty())
					{
						continue;
					}

					FrontNotificationData = std::move(NotificationsQueue.front());
					NotificationsQueue.pop();

					NotificationsLock.unlock();

					using HealthyObserversPairType = std::pair<std::shared_ptr<IObserverDispatcher>, std::shared_ptr<IRideStateObserver>>;

					std::vector<HealthyObserversPairType> HealthyObservers;

					{
						// In this scope, the expired weak pointers are cleaned up and a copy style vector of the healthy shared_ptr gets created for the callback (next step)
						std::lock_guard<std::mutex> ObserversLock(ObserversMutex);

						HealthyObservers.reserve(Observers.size());

						for (auto Iter = Observers.begin(); Iter != Observers.end();)
						{
							std::shared_ptr<IObserverDispatcher> Dispatcher = Iter->Dispatcher.lock();
							std::shared_ptr<IRideStateObserver> Observer = Iter->Observer.lock();

							if (Dispatcher && Observer)
							{
								HealthyObservers.push_back({ Dispatcher, Observer });
								++Iter;
							}
							else
							{
								Iter = Observers.erase(Iter);
							}
						}
					}

					for (const HealthyObserversPairType& CurrentObserverData : HealthyObservers)
					{
						try
						{
							CurrentObserverData.first->Dispatch(CurrentObserverData.second, FrontNotificationData);
						}
						catch (const std::exception& Exception)
						{
							std::fprintf(stderr, "Observer callback threw exception : %s\n", Exception.what());
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
			std::lock_guard<std::mutex> Lock(NotificationsMutex);
			bShuttingDown = true;
		}

		NotificationQueueCV.notify_one();

		if (ObserverThread.joinable())
		{
			ObserverThread.join();
		}
	}

	bool Subscribe(const std::shared_ptr<IObserverDispatcher>& Dispatcher, const std::shared_ptr<IRideStateObserver>&  Observer)
	{
		using TNotificationManager = TNotificationManager<std::string, std::uint64_t>;
		[[maybe_unused]] uint64_t handle = TNotificationManager::GetInstance().subscribe("asd", [](const std::string&, const void*)
			{
			});

		if (handle > 0)
		{
			TNotificationManager::GetInstance().unSubscribe(handle);
		}

		if (!Dispatcher || !Observer)
		{
			return false;
		}

		{
			std::scoped_lock Lock(ObserversMutex, NotificationsMutex);

			if (bShuttingDown)
			{
				return false;
			}

			for (auto Iter = Observers.begin(); Iter != Observers.end();)
			{
				std::shared_ptr<IObserverDispatcher> CurrentDispatcher = Iter->Dispatcher.lock();
				std::shared_ptr<IRideStateObserver> CurrentObserver = Iter->Observer.lock();

				if (CurrentDispatcher && CurrentObserver)
				{
					if (CurrentDispatcher == Dispatcher && CurrentObserver == Observer)
					{
						return false;
					}
					else
					{
						++Iter;
					}
				}
				else
				{
					Iter = Observers.erase(Iter);
				}
			}

			Observers.push_back({ Dispatcher, Observer });

			return true;
		}
	}

	bool Unsubscribe(const std::shared_ptr<IRideStateObserver>& Observer)
	{
		if (!Observer)
		{
			return false;
		}

		{
			std::scoped_lock Lock(ObserversMutex, NotificationsMutex);

			if (bShuttingDown)
			{
				return false;
			}

			for (auto Iter = Observers.begin(); Iter != Observers.end();)
			{
				if (auto CurrentObserver = Iter->Observer.lock())
				{
					if (CurrentObserver == Observer)
					{
						Iter = Observers.erase(Iter);
					}
					else
					{
						++Iter;
					}
				}
				else
				{
					Iter = Observers.erase(Iter);
				}
			}
		}

		return true;
	}

	bool ResetToIdle() { return TryChangingState(RideState::Idle); }
    bool RequestRide() { return TryChangingState(RideState::Requested); }
    bool StartBoarding() { return TryChangingState(RideState::PassengerBoarding); }
    bool StartRide() { return TryChangingState(RideState::InProgress); }
    bool CompleteRide() { return TryChangingState(RideState::Completed); }

    RideState GetCurrentState() const
	{
		std::lock_guard<std::mutex> Lock(RideStateMutex);
		return CurrentRideState;
	}

private:
    RideState CurrentRideState = RideState::Idle;
	mutable std::mutex RideStateMutex;

    std::vector<ObserverData> Observers;
    std::mutex ObserversMutex;

	std::thread ObserverThread;

	std::queue<NotificationData> NotificationsQueue;
	std::condition_variable NotificationQueueCV;
	std::mutex NotificationsMutex;
	bool bShuttingDown = false;

	static constexpr std::size_t MaxPendingNotifications = 1024;

	bool TryChangingState(const RideState NextRideState)
	{
		RideState OldRideState;

		{
			std::scoped_lock Lock(RideStateMutex, NotificationsMutex);

			if (!IsValidTransition(CurrentRideState, NextRideState) || (NotificationsQueue.size() >= MaxPendingNotifications) || bShuttingDown)
			{
				return false;
			}

			OldRideState = CurrentRideState;
			CurrentRideState = NextRideState;

			NotificationsQueue.push({ OldRideState, NextRideState });

			NotificationQueueCV.notify_one();
		}

		return true;
	}

	static bool IsValidTransition(RideState Current, RideState Next)
	{
		switch (Next)
		{
		case RideState::Idle:
		{
			if (Current == RideState::Idle) return false;
			break;
		}
		case RideState::Requested:
		{
			if (Current != RideState::Idle) return false;
			break;
		}
		case RideState::PassengerBoarding:
		{
			if (Current != RideState::Requested) return false;
			break;
		}
		case RideState::InProgress:
		{
			if (Current != RideState::PassengerBoarding) return false;
			break;
		}
		case RideState::Completed:
		{
			if (Current != RideState::InProgress) return false;
			break;
		}
		}

		return true;
	}
};
