# Enable testing
enable_testing()

# Test directories
set(TEST_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/headers")
set(TEST_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sources/scripts")

# Explicit test source list (no file(GLOB)) for reproducible builds.
set(TEST_SOURCES
    sources/test_binding_registry.cpp
    sources/test_error.cpp
    sources/test_lifecycle.cpp
    sources/test_script_runtime.cpp
)

# Fetch GoogleTest
include(FetchContent)
FetchContent_Declare(
    googletest
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/googletest
)
# Prevent GoogleTest from overriding our compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# maïs is the package being installed: a test dependency must not add its own
# headers, libraries or documentation to that installation.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Create test executable
add_executable(test_${PROJECT_NAME} ${TEST_SOURCES})

# Configure include directories
target_include_directories(test_${PROJECT_NAME} PRIVATE
    ${TEST_HEADERS_DIR}
)

# The tests play the role of a host: they define bindings with pybind11 and
# need to know where the fixture scripts live.
target_compile_definitions(test_${PROJECT_NAME} PRIVATE
    MAIS_TEST_SCRIPT_DIR="${TEST_SCRIPTS_DIR}"
)

# Link libraries. pybind11 is linked explicitly because maïs keeps it private.
target_link_libraries(test_${PROJECT_NAME} PRIVATE
    mais
    pybind11::embed
    gtest_main
)

# Run the whole suite as a single test as well, so it cannot be skipped
# silently. gtest_discover_tests() writes nothing when discovery fails, and with
# a multi-config generator used without `ctest -C` it registers no tests at all;
# in either case CTest would otherwise report success with none of the unit
# tests having run.
add_test(NAME ${PROJECT_NAME}_unit_tests
    COMMAND test_${PROJECT_NAME} --gtest_fail_if_no_test_linked
)

# Discover and register each test individually.
include(GoogleTest)
gtest_discover_tests(test_${PROJECT_NAME})
