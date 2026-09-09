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
class TExpiringCache
{
public:
	TExpiringCache();
	~TExpiringCache();

	void put(const Key& key, Value value, std::chrono::milliseconds ttl);
	// For this exercise, we return an optional<Value> even though it copies the stored Value object and hence could result in performance drops with hot get() calls. Alternatives being shared_ptr<const Value> & bool tryGet(key, out)
	std::optional<Value> get(const Key& key) const;
	bool remove(const Key& key);
	// Returns physical map size, not valid - live - entry count
	size_t size() const;
	void stop();

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

	uint64_t currentGeneration_ = 0;
	std::unordered_map<Key, MapEntry> lookupMap_;
	std::priority_queue<HeapEntry, std::vector<HeapEntry>, ExpirationCompare> expirationQueue_;

	mutable std::shared_mutex dataMutex_;
	// Intentionally embracing the extra overhead of std::condition_variable_any over std::condition_variable in order to be able to leverage std::shared_mutex for potentially extreme read calls (get(), size() etc.)
	std::condition_variable_any expirationQueueCv_;
	bool isStopping_ = false;

	std::thread expirationHandlerThread_;
};


template<typename Key, typename Value>
TExpiringCache<Key, Value>::TExpiringCache()
{
	expirationHandlerThread_ = std::thread([this]()
		{
			std::unique_lock<std::shared_mutex> lock(dataMutex_);

			while (true)
			{
				std::chrono::steady_clock::time_point timeToWaitUntil;

				timeToWaitUntil = expirationQueue_.empty() ? std::chrono::steady_clock::time_point::max() : expirationQueue_.top().expiration;

				expirationQueueCv_.wait_until(lock, timeToWaitUntil, [this]
					{
						return isStopping_ || (!expirationQueue_.empty() && expirationQueue_.top().expiration <= std::chrono::steady_clock::now());
					});

				if (isStopping_)
				{
					break;
				}

				const auto now = std::chrono::steady_clock::now();
				while (!expirationQueue_.empty() && expirationQueue_.top().expiration <= now)
				{
					const HeapEntry expiredData = expirationQueue_.top();
					expirationQueue_.pop();
					if (auto foundElement = lookupMap_.find(expiredData.key); foundElement != lookupMap_.end() && foundElement->second.generation == expiredData.generation)
					{
						lookupMap_.erase(foundElement);
					}
				}
			}
		});
}

template<typename Key, typename Value>
TExpiringCache<Key, Value>::~TExpiringCache()
{
	stop();
}

template<typename Key, typename Value>
void TExpiringCache<Key, Value>::put(const Key& key, Value value, std::chrono::milliseconds ttl)
{
	std::unique_lock<std::shared_mutex> lock(dataMutex_);

	if (isStopping_)
	{
		return;
	}

	auto lookupElement = lookupMap_.insert_or_assign(
		key,
		MapEntry{ ExpirationMetadata { std::chrono::steady_clock::now() + ttl, ++currentGeneration_ }, std::move(value) }
	);

	expirationQueue_.push(HeapEntry{ ExpirationMetadata { lookupElement.first->second.expiration, lookupElement.first->second.generation }, key });

	if (expirationQueue_.top().key == key)
	{
		expirationQueueCv_.notify_one();
	}
}

template<typename Key, typename Value>
std::optional<Value> TExpiringCache<Key, Value>::get(const Key& key) const
{
	std::shared_lock<std::shared_mutex> lock(dataMutex_);

	if (isStopping_)
	{
		return std::nullopt;
	}

	const auto foundElement = lookupMap_.find(key);

	if (foundElement == lookupMap_.end())
	{
		return std::nullopt;
	}

	if (foundElement->second.expiration <= std::chrono::steady_clock::now())
	{
		return std::nullopt;
	}

	return std::optional<Value>(foundElement->second.value);
}

template<typename Key, typename Value>
bool TExpiringCache<Key, Value>::remove(const Key& key)
{
	std::unique_lock<std::shared_mutex> lock(dataMutex_);

	if (isStopping_)
	{
		return false;
	}

	size_t removedCount = lookupMap_.erase(key);

	if (!expirationQueue_.empty() && expirationQueue_.top().key == key)
	{
		expirationQueueCv_.notify_one();
	}

	return (removedCount > 0);
}

template<typename Key, typename Value>
size_t TExpiringCache<Key, Value>::size() const
{
	std::shared_lock<std::shared_mutex> lock(dataMutex_);

	if (isStopping_)
	{
		return 0;
	}

	return lookupMap_.size();
}

template<typename Key, typename Value>
void TExpiringCache<Key, Value>::stop()
{
	{
		std::unique_lock<std::shared_mutex> lock(dataMutex_);

		isStopping_ = true;
	}

	expirationQueueCv_.notify_all();

	if (expirationHandlerThread_.joinable())
	{
		expirationHandlerThread_.join();
	}
}
