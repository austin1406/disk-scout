cmake_minimum_required(VERSION 3.16)
project(disk_scout VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(diskscout_core src/scanner.cpp src/format.cpp)
target_include_directories(diskscout_core PUBLIC src)
target_link_libraries(diskscout_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(diskscout_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(diskscout_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(disk-scout src/main.cpp)
target_link_libraries(disk-scout PRIVATE diskscout_core)
if(MSVC)
  target_compile_options(disk-scout PRIVATE /W4 /permissive-)
else()
  target_compile_options(disk-scout PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(DISKSCOUT_BUILD_TESTS "Build the unit tests" ON)
if(DISKSCOUT_BUILD_TESTS)
  include(FetchContent)
  # Use the same C runtime as the rest of the project on MSVC.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(diskscout_tests
    tests/test_format.cpp
    tests/test_thread_pool.cpp
    tests/test_scanner.cpp)
  target_link_libraries(diskscout_tests PRIVATE diskscout_core GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(diskscout_tests)
endif()
