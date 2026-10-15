cmake_minimum_required(VERSION 3.16)
project(quant_sys CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(QUANT_NATIVE_ARCH "Use -march=native. For WSL latency benches only; off for portable Docker builds." OFF)
option(QUANT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer." OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Portable by default. -march=native measures the build CPU, not a stable target.
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

if(QUANT_NATIVE_ARCH)
  add_compile_options(-march=native)
else()
  add_compile_options(-march=x86-64-v3)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

if(QUANT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

# Run records cite the commit that produced them. Re-configure when HEAD or
# the index moves so the generated header does not go stale.
set(QUANT_GIT_COMMIT "unknown")
set(QUANT_GIT_DIRTY 1)
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE git_head OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE git_head_rc ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE git_status RESULT_VARIABLE git_status_rc ERROR_QUIET)
  if(git_head_rc EQUAL 0)
    set(QUANT_GIT_COMMIT "${git_head}")
  endif()
  if(git_status_rc EQUAL 0 AND git_status STREQUAL "")
    set(QUANT_GIT_DIRTY 0)
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_SOURCE_DIR}/.git/HEAD" "${CMAKE_SOURCE_DIR}/.git/index")
endif()
configure_file(cmake/build_info.hpp.in ${CMAKE_BINARY_DIR}/generated/quant_build_info.hpp @ONLY)

# Test binaries keep assert() enabled. Release benches and replays define NDEBUG.
function(quant_add name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "TEST" "" "LIBS")
  add_executable(${name} src/${name}.cpp)
  target_include_directories(${name} PRIVATE include ${CMAKE_BINARY_DIR}/generated)
  if(ARG_LIBS)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
  endif()
  if(ARG_TEST)
    target_compile_options(${name} PRIVATE -Werror)
    add_test(NAME ${name} COMMAND ${name})
  else()
    target_compile_definitions(${name} PRIVATE $<$<CONFIG:Release>:NDEBUG>)
  endif()
endfunction()

quant_add(bench_ring_buffer LIBS pthread)
quant_add(test_itch_parser TEST)
quant_add(replay_itch)
quant_add(test_order_pool TEST)
quant_add(test_occupied_ticks TEST)
quant_add(test_order_book TEST)
quant_add(replay_symbol)
quant_add(test_matching_engine TEST)
quant_add(bench_matching_engine LIBS pthread)
quant_add(bench_gap_scan)
quant_add(test_risk_engine TEST)
quant_add(test_risk_gated_engine TEST)
quant_add(test_symbol_router TEST)
quant_add(replay_full_market LIBS pthread)
quant_add(test_multi_symbol_book TEST)
quant_add(bench_lookup_cost)
quant_add(test_locate_indexed_book TEST)
quant_add(test_imbalance_study TEST)
quant_add(research_imbalance)
quant_add(test_itch_fuzz TEST)
quant_add(test_golden_replay TEST)
quant_add(test_run_record TEST)
quant_add(test_capacity_plan TEST)
quant_add(test_fill_model TEST)
quant_add(test_features TEST)
quant_add(test_evaluate TEST)
quant_add(research_evaluate)
quant_add(test_sharded_replay TEST LIBS pthread)
