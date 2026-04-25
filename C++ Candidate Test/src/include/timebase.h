
#pragma once
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <thread>

namespace timebase {
inline void run(std::atomic<bool>& stop, bool) {
  while (!stop.load()) {
    auto t1 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto t2 = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t2.time_since_epoch()).count()
            - std::chrono::duration_cast<std::chrono::microseconds>(t1.time_since_epoch()).count();
    spdlog::info("latency={} us", us);
  }
}
}
