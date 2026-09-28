cmake_minimum_required(VERSION 3.20)
project(exchange_sim VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(EXSIM_NATIVE   "Tune for the build machine (-march=native)" ON)
option(EXSIM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(EXSIM_TSAN     "Build with ThreadSanitizer" OFF)
option(EXSIM_WERROR   "Treat warnings as errors" OFF)

find_package(Threads REQUIRED)

# The engine: header-only, no third-party dependencies.
add_library(exsim INTERFACE)
add_library(exsim::exsim ALIAS exsim)
target_include_directories(exsim INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(exsim INTERFACE cxx_std_20)
target_link_libraries(exsim INTERFACE Threads::Threads)

# Build flags shared by every binary in this repository.
add_library(exsim_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(exsim_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
                                                -Wcast-align -Wimplicit-fallthrough)
  if(EXSIM_WERROR)
    target_compile_options(exsim_flags INTERFACE -Werror)
  endif()
  if(EXSIM_NATIVE AND NOT EXSIM_SANITIZE AND NOT EXSIM_TSAN)
    target_compile_options(exsim_flags INTERFACE -march=native)
  endif()
  if(EXSIM_SANITIZE)
    target_compile_options(exsim_flags INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(exsim_flags INTERFACE -fsanitize=address,undefined)
  endif()
  if(EXSIM_TSAN)
    target_compile_options(exsim_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(exsim_flags INTERFACE -fsanitize=thread)
  endif()
endif()

function(exsim_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE exsim exsim_flags)
endfunction()

exsim_executable(exsim_gen      tools/exsim_gen.cpp)
exsim_executable(exsim_replay   tools/exsim_replay.cpp)
exsim_executable(exsim_pipeline tools/exsim_pipeline.cpp)
exsim_executable(exsim_mdreplay tools/exsim_mdreplay.cpp)
exsim_executable(exsim_mmsim    tools/exsim_mmsim.cpp)
exsim_executable(exsim_l3replay tools/exsim_l3replay.cpp)
exsim_executable(exsim_queuestudy tools/exsim_queuestudy.cpp)
exsim_executable(exsim_difffeed tools/exsim_difffeed.cpp)
exsim_executable(exsim_features tools/exsim_features.cpp)
exsim_executable(exsim_bench    bench/exsim_bench.cpp)
exsim_executable(exsim_bench_index bench/bench_index_deletion.cpp)

exsim_executable(exsim_tests
  tests/test_main.cpp
  tests/test_book.cpp
  tests/test_components.cpp
  tests/test_differential.cpp
  tests/test_alloc.cpp
  tests/test_mm.cpp
  tests/test_journal_risk.cpp)

enable_testing()
add_test(NAME exsim_tests COMMAND exsim_tests)
set_tests_properties(exsim_tests PROPERTIES TIMEOUT 600)  # a corrupted FIFO can loop forever

# Optional head-to-head benchmark against Liquibook (scripts/fetch_liquibook.sh).
set(EXSIM_LIQUIBOOK_DIR "" CACHE PATH "Path to a Liquibook checkout; enables exsim_bench_liquibook")
if(EXSIM_LIQUIBOOK_DIR AND EXISTS "${EXSIM_LIQUIBOOK_DIR}/src/book/order_book.h")
  add_executable(exsim_bench_liquibook bench/bench_liquibook.cpp "${EXSIM_LIQUIBOOK_DIR}/src/simple/simple_order.cpp")
  target_include_directories(exsim_bench_liquibook SYSTEM PRIVATE "${EXSIM_LIQUIBOOK_DIR}/src")
  target_link_libraries(exsim_bench_liquibook PRIVATE exsim exsim_flags)
  # Liquibook's listener base classes have non-virtual destructors; silence that for this target only
  # (a source property is applied after the interface flags, so it wins over -Wnon-virtual-dtor).
  set_property(SOURCE bench/bench_liquibook.cpp "${EXSIM_LIQUIBOOK_DIR}/src/simple/simple_order.cpp"
               APPEND PROPERTY COMPILE_OPTIONS -Wno-non-virtual-dtor)
endif()

# Order-entry gateway: needs epoll, so Linux only.
exsim_executable(exsim_journal tools/exsim_journal.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  exsim_executable(exsim_server tools/exsim_server.cpp)
  exsim_executable(exsim_client tools/exsim_client.cpp)
  exsim_executable(exsim_replica tools/exsim_replica.cpp)
endif()
