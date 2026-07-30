#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>

#include "generic_singelton.h"


template<typename Type>
concept Hashable =
	requires(const Type& value)
	{
		{
			std::hash<Type>{}(value)
		} -> std::convertible_to<std::size_t>;
	};

template<typename Type>
concept IncrementableHandle =
	requires(Type value)
{
	{
		++value
	} -> std::same_as<Type&>;
};


// T being the notifications' key type and K being notifications' handle type
template<typename T = std::string, typename K = std::uint64_t>
	requires
		Hashable<T> &&
		std::equality_comparable<T> &&
		Hashable<K> &&
		std::equality_comparable<K> &&
		IncrementableHandle<K>
class TNotificationManager final : public TSingleton<TNotificationManager<T, K>>
{
	DEFINESINGELTON(TNotificationManager);

public:

	using TCallback = std::function<void(const T& key, const void* payload)>;

	K subscribe(const T& key, TCallback callback)
	{
		++latestHandle;

		const auto [handleIter, handleInserted] =
			handleToKeyMap.try_emplace(
				latestHandle,
				key);
		assert(handleInserted);

		const auto [callbackIter, callbackInserted] =
			keyToHandleToCallbackMap[key].try_emplace(
				latestHandle,
				std::move(callback));
		assert(callbackInserted);

		return latestHandle;
	}

	bool unsubscribe(const K& handle)
	{
		if (const auto foundHandleKeyPair = handleToKeyMap.find(handle); foundHandleKeyPair != handleToKeyMap.end())
		{
			if (const auto handleToCallbacksMap = keyToHandleToCallbackMap.find(foundHandleKeyPair->second); handleToCallbacksMap != keyToHandleToCallbackMap.end())
			{
				handleToCallbacksMap->second.erase(handle);

				if (handleToCallbacksMap->second.empty())
				{
					keyToHandleToCallbackMap.erase(handleToCallbacksMap);
				}
			}

			handleToKeyMap.erase(foundHandleKeyPair);

			return true;
		}

		return false;
	}

	void notify(const T& key, const void* payload)
	{
		if (const auto foundCallbackSet = keyToHandleToCallbackMap.find(key); foundCallbackSet != keyToHandleToCallbackMap.end())
		{
			const auto handleAndCallbacksCopy = foundCallbackSet->second;
			for (const auto& currentHandleAndCallback : handleAndCallbacksCopy)
			{
				currentHandleAndCallback.second(key, payload);
			}
		}
	}

private:

	K latestHandle { K() };
	std::unordered_map<K, T> handleToKeyMap;
	std::unordered_map<T, std::unordered_map<K, TCallback>> keyToHandleToCallbackMap;

};
