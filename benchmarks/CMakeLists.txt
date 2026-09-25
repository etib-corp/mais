# Benchmark harness for maïs.
#
# Built only when BUILD_BENCHMARKS=ON. Uses Google Benchmark to measure the
# cost of invoking a script, which is the one maïs operation a host calls every
# frame.

include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/googlebenchmark
)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
# Keep the benchmark harness out of the installed maïs package.
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(mais_benchmarks
    benchmark_main.cpp
)

target_link_libraries(mais_benchmarks PRIVATE
    mais
    pybind11::embed
    benchmark::benchmark
)

target_compile_definitions(mais_benchmarks PRIVATE
    MAIS_BENCHMARK_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts"
)

target_include_directories(mais_benchmarks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
