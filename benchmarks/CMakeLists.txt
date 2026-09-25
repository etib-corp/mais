# Benchmark harness for Mais.
#
# Built only when BUILD_BENCHMARKS=ON. Uses Google Benchmark to measure the key
# hot paths: render-object creation, mesh/material setup, and the frame loop.

include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/googlebenchmark
)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(mais_benchmarks
    benchmark_main.cpp
)

target_link_libraries(mais_benchmarks PRIVATE
    mais
    benchmark::benchmark
)

target_include_directories(mais_benchmarks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
