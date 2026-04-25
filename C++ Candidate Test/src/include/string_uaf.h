
#pragma once
#include <spdlog/spdlog.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace string_uaf {

struct DeferredLog {
  const char* ptr;
  size_t len;

  void set() {
    std::string tmp = "uaf:" + std::to_string(std::rand());
    ptr = tmp.data();
    len = tmp.size();
  }

  void emit() const {
    spdlog::info("deferred={}", std::string_view(ptr, len));
  }
};

inline void run(std::atomic<bool>& stop, bool stress) {
  while (!stop.load()) {
    DeferredLog d;
    d.set();
    std::this_thread::sleep_for(std::chrono::microseconds(stress ? 50 : 500));
    d.emit();
    std::this_thread::sleep_for(std::chrono::milliseconds(stress ? 1 : 10));
  }
}
}
