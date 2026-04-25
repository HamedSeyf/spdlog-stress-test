
A tiny project that uses spdlog.
Build:
  cmake -S . -B build
  cmake --build build -j
  ./build/spdlogger --seconds 5 (optional --stress)


Your task 
Identify and fix as many correctness issues as you reasonably can. 
For each fix, briefly explain: 
what the problem was 
why it happens (especially why it’s intermittent) 
why your fix is correct 

Constraints 
Do not remove async logging. 

Upload solutions to github with Readme 