
#pragma once
#include <spdlog/spdlog.h>
#include <atomic>
#include <thread>

namespace shutdown {
inline void run(std::atomic<bool>& stop, bool stress) {
  spdlog::logger* raw = spdlog::default_logger_raw();
  std::thread t([&] {
    while (!stop.load()) raw->info("shutdown now");
  });

  if (stress) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    spdlog::shutdown();
  }

  stop.store(true);
  t.join();
}
}
