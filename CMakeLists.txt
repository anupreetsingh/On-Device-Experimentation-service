cmake_minimum_required(VERSION 3.20)
project(ExperimentationService LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(EXP_BUILD_TESTS "Build the GoogleTest suite" ON)
option(EXP_TSAN "Build everything with ThreadSanitizer" OFF)

if(EXP_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
set(EXP_WARNINGS -Wall -Wextra)

# expd's pieces, as a library so the tests can link them.
add_library(expd_core STATIC
  expd/sha256.cpp
  expd/assignment.cpp
  expd/files.cpp
  expd/device_id.cpp
)
target_include_directories(expd_core PUBLIC expd)
target_compile_options(expd_core PRIVATE ${EXP_WARNINGS})
target_link_libraries(expd_core PUBLIC Threads::Threads)

if(EXP_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  add_executable(expd_tests
    tests/assignment_test.cpp
  )
  target_link_libraries(expd_tests PRIVATE expd_core GTest::gtest_main)
  target_compile_options(expd_tests PRIVATE ${EXP_WARNINGS})
  gtest_discover_tests(expd_tests)
endif()
