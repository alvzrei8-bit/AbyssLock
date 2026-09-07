cmake_minimum_required(VERSION 3.20)
project(abysslock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(abysslock abysslock.cpp)

if (MSVC)
  target_compile_options(abysslock PRIVATE /W4 /O2)
else()
  target_compile_options(abysslock PRIVATE -Wall -Wextra -Wpedantic -O2)
endif()