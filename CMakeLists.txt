cmake_minimum_required(VERSION 3.16)
project(quant_sys CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_executable(bench_ring_buffer src/bench_ring_buffer.cpp)
target_include_directories(bench_ring_buffer PRIVATE include)
target_link_libraries(bench_ring_buffer pthread)

add_executable(test_itch_parser src/test_itch_parser.cpp)
target_include_directories(test_itch_parser PRIVATE include)

add_executable(replay_itch src/replay_itch.cpp)
target_include_directories(replay_itch PRIVATE include)

add_executable(test_order_pool src/test_order_pool.cpp)
target_include_directories(test_order_pool PRIVATE include)

add_executable(test_order_book src/test_order_book.cpp)
target_include_directories(test_order_book PRIVATE include)

add_executable(replay_symbol src/replay_symbol.cpp)
target_include_directories(replay_symbol PRIVATE include)

add_executable(test_matching_engine src/test_matching_engine.cpp)
target_include_directories(test_matching_engine PRIVATE include)

add_executable(bench_matching_engine src/bench_matching_engine.cpp)
target_include_directories(bench_matching_engine PRIVATE include)
target_link_libraries(bench_matching_engine pthread)

add_executable(test_risk_engine src/test_risk_engine.cpp)
target_include_directories(test_risk_engine PRIVATE include)


add_executable(test_risk_gated_engine src/test_risk_gated_engine.cpp)
target_include_directories(test_risk_gated_engine PRIVATE include)

add_executable(test_symbol_router src/test_symbol_router.cpp)
target_include_directories(test_symbol_router PRIVATE include)

add_executable(replay_full_market src/replay_full_market.cpp)
target_include_directories(replay_full_market PRIVATE include)

add_executable(test_multi_symbol_book src/test_multi_symbol_book.cpp)
target_include_directories(test_multi_symbol_book PRIVATE include)

add_executable(bench_lookup_cost src/bench_lookup_cost.cpp)
target_include_directories(bench_lookup_cost PRIVATE include)

add_executable(test_locate_indexed_book src/test_locate_indexed_book.cpp)
target_include_directories(test_locate_indexed_book PRIVATE include)
