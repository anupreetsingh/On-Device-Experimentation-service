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
