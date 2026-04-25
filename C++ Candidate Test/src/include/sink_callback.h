
#pragma once
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <thread>

namespace sink_callback {

class ReentrantSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    spdlog::info("re-enter logger");
  }
  void flush_() override {}
};

inline void run(std::atomic<bool>& stop, bool stress) {
  if (!stress) return;
  auto sink = std::make_shared<ReentrantSink>();
  auto logger = std::make_shared<spdlog::logger>("reentrant", sink);
  while (!stop.load()) {
    logger->warn("trigger");
  }
}
}
