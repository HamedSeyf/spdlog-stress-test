A tiny project that uses spdlog.
Build:
  cmake -S . -B build
  cmake --build build -j
  ./build/spdlogger --seconds 5 (optional --stress)
