# A host that models the shape a real XR/UI consumer uses: a validated
# settings facade, a native per-frame context, a fixed timestep, per-hook
# failure policies, and a shutdown summary. It depends on no engine, renderer,
# window, or UI toolkit.
add_executable(frame_loop_host main.cpp)

target_link_libraries(frame_loop_host PRIVATE
    mais
    pybind11::embed
)

# The example is the M2 acceptance test. The normal run must survive a
# deliberately failing hook; the second run must reject an invalid settings
# proposal without applying it.
if(BUILD_TESTING)
    add_test(NAME frame_loop_host
        COMMAND frame_loop_host "${CMAKE_CURRENT_SOURCE_DIR}/scripts" game
    )
    set_tests_properties(frame_loop_host PROPERTIES
        PASS_REGULAR_EXPRESSION
            "frames=120/120.*update_reports=1 suppressed=1.*settings=1600x900"
    )

    add_test(NAME frame_loop_host_invalid_settings
        COMMAND frame_loop_host
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts" invalid_settings_game
    )
    set_tests_properties(frame_loop_host_invalid_settings PROPERTIES
        PASS_REGULAR_EXPRESSION
            "rejected settings proposal: width must be between 64 and 7680"
        FAIL_REGULAR_EXPRESSION
            "settings=0x720;engine started .*invalid frame loop host"
    )
endif()
