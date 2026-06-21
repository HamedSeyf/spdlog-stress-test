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

#include <spdlog/spdlog.h>


template<typename T, typename TComparator = std::less<T>>
void BubbleSort(std::vector<T>& Input, const TComparator& Comparator = TComparator())
{
	if (Input.size() < 2)
	{
		return;
	}

	for (std::size_t OuterElementIndexToFix = Input.size() - 1; OuterElementIndexToFix > 0; --OuterElementIndexToFix)
	{
		bool bHasSwappedAnyElement = false;

		for (std::size_t InnerElementIndex = 0; InnerElementIndex < OuterElementIndexToFix; ++InnerElementIndex)
		{
			if (Comparator(Input[InnerElementIndex + 1], Input[InnerElementIndex]))
			{
				std::swap(Input[InnerElementIndex], Input[InnerElementIndex + 1]);
				bHasSwappedAnyElement = true;
			}
		}

		if (!bHasSwappedAnyElement)
		{
			break;
		}
	}
}


template<typename T, typename TComparator>
[[nodiscard]] std::vector<T> MergeSortedVectors(const std::vector<T>& Input1, const std::vector<T>& Input2, const TComparator& Comparator)
{
	std::vector<T> RetVal;
	RetVal.reserve(Input1.size() + Input2.size());

	auto Iter1 = Input1.begin();
	auto Iter2 = Input2.begin();

	while (Iter1 != Input1.end() || Iter2 != Input2.end())
	{
		if (Iter1 == Input1.end())
		{
			RetVal.push_back(*Iter2++);
		}
		else if (Iter2 == Input2.end())
		{
			RetVal.push_back(*Iter1++);
		}
		else if (Comparator(*Iter2, *Iter1))
		{
			RetVal.push_back(*Iter2++);
		}
		else
		{
			RetVal.push_back(*Iter1++);
		}
	}

	return RetVal;
}


template<typename T, typename TComparator = std::less<T>>
[[nodiscard]] std::vector<T> MergeSort(const std::vector<T>& Input, std::size_t StartIndex, std::size_t EndIndex, const TComparator& Comparator = TComparator())
{
	assert(StartIndex <= EndIndex);
	assert(Input.empty() || EndIndex < Input.size());

	if (Input.empty())
	{
		return {};
	}

	if (StartIndex == EndIndex)
	{
		return std::vector<T>(Input.begin() + StartIndex, Input.begin() + EndIndex + 1);
	}

	// Avoiding (StartIndex + EndIndex) / 2 to avoid potential overflow of sum operation
	const std::size_t MidIndex = StartIndex + ((EndIndex - StartIndex) / 2);

	const std::vector<T> LeftSortedVector = MergeSort(Input, StartIndex, MidIndex, Comparator);
	const std::vector<T> RightSortedVector = MergeSort(Input, MidIndex + 1, EndIndex, Comparator);

	return MergeSortedVectors(LeftSortedVector, RightSortedVector, Comparator);
}


// Simple overload which sorts the whole array
template<typename T, typename TComparator = std::less<T>>
[[nodiscard]] std::vector<T> MergeSort(const std::vector<T>& Input, const TComparator& Comparator = TComparator())
{
	return MergeSort(Input, 0, Input.size() - 1, Comparator);
}
