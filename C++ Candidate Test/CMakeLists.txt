
cmake_minimum_required(VERSION 3.20)
project(spdlog_misuse_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
)
FetchContent_MakeAvailable(spdlog)

add_executable(spdlogger
  src/main.cpp
)

target_link_libraries(spdlogger PRIVATE spdlog::spdlog)

if (MSVC)
  target_compile_options(spdlogger PRIVATE /W4)
else()
  target_compile_options(spdlogger PRIVATE -Wall -Wextra -Wpedantic)
endif()
