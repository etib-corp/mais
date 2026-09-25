# A host with no engine and no UI: it owns two native services, embeds Python
# through maïs, and drives the script hooks from its own loop.
add_executable(standalone_host main.cpp)

target_link_libraries(standalone_host PRIVATE
    mais
    pybind11::embed
)

# The example doubles as an integration test: when tests are enabled, CTest
# runs it against the real script and checks that it exits cleanly.
if(BUILD_TESTING)
    add_test(NAME standalone_host
        COMMAND standalone_host "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
    )
endif()
