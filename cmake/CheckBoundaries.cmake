# Configure-time gate for the dependency boundary in AGENTS.md: maïs is a
# generic scripting runtime and links only pybind11 and the Python runtime that
# pybind11 exposes.
#
# The check asserts the positive property instead of naming forbidden projects:
# the transitive link interface may contain nothing but pybind11 targets, Python
# targets, and the Python library file. An engine, a renderer, or a UI toolkit
# is rejected whether or not this file has heard of it.
#
# The gate is scheduled for the end of the top-level directory so that a
# consumer which adds a dependency to the target after add_subdirectory() is
# caught as well. Turning it off is a compatibility decision, not a build flag.

# The approved sets live inside the functions that use them: a deferred call
# runs in the top-level directory, where file-scope variables set here, in a
# library subdirectory, are no longer in scope. An empty regex would silently
# approve everything, which is exactly the failure this gate must not have.

# Reads a target property and normalizes CMake's <property>-NOTFOUND marker to
# an empty list, so that it is never iterated over as a library name.
function(_mais_boundary_property target property out_var)
    get_target_property(_value ${target} ${property})
    if(NOT _value)
        set(_value "")
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

# Accepts the Python library however a pybind11 helper happens to spell it:
# as a bare name, an -l flag, or a path to the shared library.
function(_mais_boundary_is_python_library library out_var)
    get_filename_component(_name "${library}" NAME)
    if("${library}" MATCHES "^-lpython"
        OR _name MATCHES "^(lib)?python[0-9._]*(d)?(\\.[a-z0-9.]+)?$"
    )
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Removes the wrappers a static library gains when it stores a private
# dependency in its interface, for example $<LINK_ONLY:pybind11::embed>.
function(_mais_boundary_unwrap expression out_var)
    set(_value "${expression}")
    set(_unwrapped TRUE)
    while(_unwrapped)
        set(_unwrapped FALSE)
        foreach(_wrapper LINK_ONLY TARGET_NAME_IF_EXISTS)
            if(_value MATCHES "^\\$<${_wrapper}:(.+)>$")
                set(_value "${CMAKE_MATCH_1}")
                set(_unwrapped TRUE)
            endif()
        endforeach()
    endwhile()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(_mais_boundary_check_target target out_var)
    set(_violations "")

    _mais_boundary_property(${target} LINK_LIBRARIES _link_libraries)
    _mais_boundary_property(${target} INTERFACE_LINK_LIBRARIES _link_interface)

    set(_direct "")
    foreach(_entry IN LISTS _link_libraries _link_interface)
        _mais_boundary_unwrap("${_entry}" _entry)
        # When a target is linked from another directory, CMake records a
        # directory marker next to the entries. It is not a dependency itself.
        if(_entry MATCHES "^::@")
            continue()
        endif()
        if(_entry)
            list(APPEND _direct "${_entry}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _direct)

    # pybind11::embed is the one approved direct dependency. Anything else is
    # reported here and is not walked, so that one wrong entry produces one
    # violation rather than a direct and a transitive copy of it.
    set(_pending "")
    foreach(_entry IN LISTS _direct)
        if(NOT _entry STREQUAL "pybind11::embed")
            list(APPEND _violations
                "'${target}' links '${_entry}' directly, and pybind11::embed is the only approved dependency"
            )
            continue()
        endif()
        list(APPEND _pending "${_entry}")
    endforeach()

    # Depth-first walk of the link interface. Interface targets such as
    # pybind11::embed carry their own dependencies, and those are what a
    # consumer ends up linking, so they are checked rather than assumed.
    set(_visited "")
    while(_pending)
        list(POP_FRONT _pending _entry)
        if(_entry IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_entry}")

        if(_entry MATCHES "^::@")
            continue()
        endif()

        if(_entry MATCHES "\\$<")
            list(APPEND _violations
                "'${target}' links '${_entry}', a generator expression this check cannot resolve, so the boundary cannot be proven"
            )
            continue()
        endif()

        # Approved names are trusted as a group. Some of pybind11's interface
        # targets are directory-scoped imports, which this check cannot see or
        # walk from the top-level scope, so the name is what can be verified.
        # Approved names: pybind11's interface targets and the runtime targets
        # CMake's FindPython creates (Python::Python, Python3::Python, ...).
        if(_entry MATCHES "^(pybind11|Python[0-9]*)::")
            # pybind11's interface is walked because it is part of the approved
            # toolchain. The Python runtime target is opaque: a static libpython
            # records its own system libraries (-lm, -ldl, -lpthread) there, and
            # those are the runtime's business, not a maïs dependency.
            if(_entry MATCHES "^pybind11::" AND TARGET "${_entry}")
                _mais_boundary_property(${_entry} LINK_LIBRARIES
                    _nested_libraries
                )
                _mais_boundary_property(${_entry} INTERFACE_LINK_LIBRARIES
                    _nested_interface
                )
                foreach(_nested IN LISTS _nested_libraries _nested_interface)
                    _mais_boundary_unwrap("${_nested}" _nested)
                    if(_nested)
                        list(APPEND _pending "${_nested}")
                    endif()
                endforeach()
            endif()
            continue()
        endif()

        if(TARGET "${_entry}")
            list(APPEND _violations
                "'${target}' links target '${_entry}', which is neither a pybind11 target nor a Python runtime target"
            )
            continue()
        endif()

        _mais_boundary_is_python_library("${_entry}" _is_python_library)
        if(NOT _is_python_library)
            list(APPEND _violations
                "'${target}' links '${_entry}', which is neither a pybind11 target nor the Python runtime"
            )
        endif()
    endwhile()

    set(${out_var} "${_violations}" PARENT_SCOPE)
endfunction()

# Deferred entry point. Reads the targets recorded by
# mais_schedule_boundary_check() instead of taking them as arguments, because
# cmake_language(DEFER) expands its arguments when the deferred call runs, in a
# scope where the library's PROJECT_NAME is no longer in effect.
function(mais_check_link_boundaries)
    get_property(_targets GLOBAL PROPERTY MAIS_BOUNDARY_TARGETS)
    if(NOT _targets)
        return()
    endif()

    set(_details "")
    foreach(_target IN LISTS _targets)
        _mais_boundary_check_target(${_target} _violations)
        # One line per violation, kept as text so that a message containing a
        # semicolon is not split into list elements on the way out.
        foreach(_violation IN LISTS _violations)
            string(APPEND _details "\n  - ${_violation}")
        endforeach()
    endforeach()

    if(_details)
        message(FATAL_ERROR
            "maïs boundary check failed:${_details}\n"
            "\n"
            "maïs is a generic scripting runtime. It links pybind11::embed and "
            "the Python runtime, and nothing else: engine, renderer, UI, and "
            "consumer dependencies belong to the consumer or to an adapter that "
            "depends on maïs, never to maïs itself. If a new generic "
            "dependency has been explicitly approved, update "
            "cmake/CheckBoundaries.cmake and AGENTS.md together."
        )
    endif()
endfunction()

# Records the library target and schedules the check for the end of the
# top-level directory, which is after every consumer has had a chance to add
# dependencies to it.
function(mais_schedule_boundary_check target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "mais_schedule_boundary_check() was given '${target}', which is not a target"
        )
    endif()
    set_property(GLOBAL APPEND PROPERTY MAIS_BOUNDARY_TARGETS "${target}")
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL mais_check_link_boundaries
    )
endfunction()
