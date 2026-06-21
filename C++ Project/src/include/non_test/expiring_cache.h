#pragma once

#include <chrono>
#include <thread>
#include <condition_variable>
#include <shared_mutex>
#include <queue>
#include <vector>
#include <optional>
#include <unordered_map>
#include <utility>
#include <cstddef>
#include <cstdint>


template<typename Key, typename Value>
class ExpiringCache
{
public:
	ExpiringCache();
	~ExpiringCache();

	void Put(const Key& key, Value value, std::chrono::milliseconds ttl);
	// For this exercise, we return an optional<Value> even though it copies the stored Value object and hence could result in performance drops with hot Get() calls. Alternatives being shared_ptr<const Value> & bool TryGet(key, out)
	std::optional<Value> Get(const Key& key) const;
	bool Remove(const Key& key);
	// Returns physical map size, not valid - live - entry count
	size_t Size() const;
	void Stop();

private:

	struct ExpirationMetadata
	{
		std::chrono::steady_clock::time_point expiration;
		uint64_t generation;
	};

	struct MapEntry : public ExpirationMetadata
	{
		Value value;
	};

	struct HeapEntry : public ExpirationMetadata
	{
		Key key;
	};

	struct ExpirationCompare
	{
		bool operator()(const HeapEntry& lhs, const HeapEntry& rhs) const
		{
			return lhs.expiration > rhs.expiration;
		}
	};

	uint64_t CurrentGeneration = 0;
	std::unordered_map<Key, MapEntry> LookupMap;
	std::priority_queue<HeapEntry, std::vector<HeapEntry>, ExpirationCompare> ExpirationQueue;

	mutable std::shared_mutex DataMutex;
	// Intentionally embracing the extra overhead of std::condition_variable_any over std::condition_variable in order to be able to leverage std::shared_mutex for potentially extreme read calls (Get(), Size() etc.)
	std::condition_variable_any ExpirationQueueCV;
	bool bIsStopping = false;

	std::thread ExpirationHandlerThread;
};


template<typename Key, typename Value>
ExpiringCache<Key, Value>::ExpiringCache()
{
	ExpirationHandlerThread = std::thread([this]()
		{
			std::unique_lock<std::shared_mutex> Lock(DataMutex);

			while (true)
			{
				std::chrono::steady_clock::time_point TimeToWaitUntil;

				TimeToWaitUntil = ExpirationQueue.empty() ? std::chrono::steady_clock::time_point::max() : ExpirationQueue.top().expiration;

				ExpirationQueueCV.wait_until(Lock, TimeToWaitUntil, [this]
					{
						return bIsStopping || (!ExpirationQueue.empty() && ExpirationQueue.top().expiration <= std::chrono::steady_clock::now());
					});

				if (bIsStopping)
				{
					break;
				}

				const auto now = std::chrono::steady_clock::now();
				while (!ExpirationQueue.empty() && ExpirationQueue.top().expiration <= now)
				{
					const HeapEntry ExpiredData = ExpirationQueue.top();
					ExpirationQueue.pop();
					if (auto FoundElement = LookupMap.find(ExpiredData.key); FoundElement != LookupMap.end() && FoundElement->second.generation == ExpiredData.generation)
					{
						LookupMap.erase(FoundElement);
					}
				}
			}
		});
}

template<typename Key, typename Value>
ExpiringCache<Key, Value>::~ExpiringCache()
{
	Stop();
}

template<typename Key, typename Value>
void ExpiringCache<Key, Value>::Put(const Key& key, Value value, std::chrono::milliseconds ttl)
{
	std::unique_lock<std::shared_mutex> Lock(DataMutex);

	if (bIsStopping)
	{
		return;
	}

	auto LookupElement = LookupMap.insert_or_assign(
		key,
		MapEntry{ ExpirationMetadata { std::chrono::steady_clock::now() + ttl, ++CurrentGeneration }, std::move(value) }
	);

	ExpirationQueue.push(HeapEntry{ ExpirationMetadata { LookupElement.first->second.expiration, LookupElement.first->second.generation }, key });

	if (ExpirationQueue.top().key == key)
	{
		ExpirationQueueCV.notify_one();
	}
}

template<typename Key, typename Value>
std::optional<Value> ExpiringCache<Key, Value>::Get(const Key& key) const
{
	std::shared_lock<std::shared_mutex> Lock(DataMutex);

	if (bIsStopping)
	{
		return std::nullopt;
	}

	const auto FoundElement = LookupMap.find(key);

	if (FoundElement == LookupMap.end())
	{
		return std::nullopt;
	}

	if (FoundElement->second.expiration <= std::chrono::steady_clock::now())
	{
		return std::nullopt;
	}

	return std::optional<Value>(FoundElement->second.value);
}

template<typename Key, typename Value>
bool ExpiringCache<Key, Value>::Remove(const Key& key)
{
	std::unique_lock<std::shared_mutex> Lock(DataMutex);

	if (bIsStopping)
	{
		return false;
	}

	size_t removedCount = LookupMap.erase(key);

	if (!ExpirationQueue.empty() && ExpirationQueue.top().key == key)
	{
		ExpirationQueueCV.notify_one();
	}

	return (removedCount > 0);
}

template<typename Key, typename Value>
size_t ExpiringCache<Key, Value>::Size() const
{
	std::shared_lock<std::shared_mutex> Lock(DataMutex);

	if (bIsStopping)
	{
		return 0;
	}

	return LookupMap.size();
}

template<typename Key, typename Value>
void ExpiringCache<Key, Value>::Stop()
{
	{
		std::unique_lock<std::shared_mutex> lock(DataMutex);

		bIsStopping = true;
	}

	ExpirationQueueCV.notify_all();

	if (ExpirationHandlerThread.joinable())
	{
		ExpirationHandlerThread.join();
	}
}
