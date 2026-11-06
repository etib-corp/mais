# Verifies the supported-range guard in cmake/PythonSupport.cmake.
#
# The guard is what turns an unsupported interpreter into an actionable
# configure error, so it is checked directly: a version below the floor and a
# version above the ceiling must both abort with a message naming the range, a
# version inside it must pass, and a missing Python_VERSION must abort instead
# of leaving the guard disabled. The range itself must also be independent of
# the order of MAIS_PYTHON_VERSIONS.

include("${MAIS_SOURCE_DIR}/cmake/PythonSupport.cmake")

set(_probe "${CMAKE_CURRENT_LIST_DIR}/guard_probe.cmake")

function(_run_guard_probe version result_var output_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DMAIS_SOURCE_DIR=${MAIS_SOURCE_DIR}"
            "-DPython_VERSION=${version}"
            "-DPython_EXECUTABLE=/nowhere/python${version}"
            -P "${_probe}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
    )
    set(${result_var} "${result}" PARENT_SCOPE)
    set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

# Derive neighbours of the declared range so the test keeps working when the
# range moves, instead of hardcoding versions that would silently go stale.
string(REPLACE "." ";" _min_parts "${MAIS_PYTHON_MIN}")
list(GET _min_parts 0 _min_major)
list(GET _min_parts 1 _min_minor)
string(REPLACE "." ";" _max_parts "${MAIS_PYTHON_MAX}")
list(GET _max_parts 0 _max_major)
list(GET _max_parts 1 _max_minor)

math(EXPR _below_minor "${_min_minor} - 1")
set(_below_version "${_min_major}.${_below_minor}.0")
math(EXPR _above_minor "${_max_minor} + 1")
set(_above_version "${_max_major}.${_above_minor}.0")

set(_expected_range "supports Python ${MAIS_PYTHON_MIN} through ${MAIS_PYTHON_MAX}")

# The floor and ceiling are the extremes of the list, not its first and last
# entries, so the order of MAIS_PYTHON_VERSIONS cannot move the range. Sorted
# input passes either way, hence the reversed case.
mais_python_supported_range("${MAIS_PYTHON_VERSIONS}" _declared_min _declared_max)
if(NOT _declared_min STREQUAL MAIS_PYTHON_MIN
   OR NOT _declared_max STREQUAL MAIS_PYTHON_MAX)
    message(FATAL_ERROR
        "The declared range (${MAIS_PYTHON_MIN}-${MAIS_PYTHON_MAX}) is not the "
        "extremes of MAIS_PYTHON_VERSIONS, which are "
        "${_declared_min}-${_declared_max}."
    )
endif()

set(_reversed_list ${MAIS_PYTHON_VERSIONS})
list(REVERSE _reversed_list)
mais_python_supported_range("${_reversed_list}" _reversed_min _reversed_max)
if(NOT _reversed_min STREQUAL MAIS_PYTHON_MIN
   OR NOT _reversed_max STREQUAL MAIS_PYTHON_MAX)
    message(FATAL_ERROR
        "Reversing MAIS_PYTHON_VERSIONS changed the supported range to "
        "${_reversed_min}-${_reversed_max}; it must stay "
        "${MAIS_PYTHON_MIN}-${MAIS_PYTHON_MAX}."
    )
endif()

foreach(_rejected IN ITEMS "${_below_version}" "${_above_version}")
    _run_guard_probe("${_rejected}" _result _output)
    if("${_result}" EQUAL 0)
        message(FATAL_ERROR
            "Python ${_rejected} was accepted, but it is outside the "
            "supported range (${MAIS_PYTHON_MIN}-${MAIS_PYTHON_MAX})."
        )
    endif()
    if(NOT _output MATCHES "${_expected_range}")
        message(FATAL_ERROR
            "Rejecting Python ${_rejected} did not name the supported range. "
            "Output was:\n${_output}"
        )
    endif()
    if(NOT _output MATCHES "${_rejected}")
        message(FATAL_ERROR
            "Rejecting Python ${_rejected} did not report the version it "
            "found. Output was:\n${_output}"
        )
    endif()
endforeach()

_run_guard_probe("${MAIS_PYTHON_MIN}.0" _result _output)
if(NOT "${_result}" EQUAL 0)
    message(FATAL_ERROR
        "Python ${MAIS_PYTHON_MIN}.0 is on the supported floor but was "
        "rejected. Output was:\n${_output}"
    )
endif()

_run_guard_probe("${MAIS_PYTHON_MAX}.0" _result _output)
if(NOT "${_result}" EQUAL 0)
    message(FATAL_ERROR
        "Python ${MAIS_PYTHON_MAX}.0 is on the supported ceiling but was "
        "rejected. Output was:\n${_output}"
    )
endif()

# A missing Python_VERSION means discovery changed shape. The guard must fail
# loudly there: a silent return would leave every interpreter accepted, and the
# synthetic versions above would keep passing, so it is exercised separately.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIS_SOURCE_DIR=${MAIS_SOURCE_DIR}"
        -P "${_probe}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _output
)
if("${_result}" EQUAL 0)
    message(FATAL_ERROR
        "mais_require_supported_python() passed with no Python_VERSION, so "
        "the guard silently disabled itself instead of reporting the broken "
        "discovery."
    )
endif()
if(NOT _output MATCHES "Python_VERSION")
    message(FATAL_ERROR
        "The guard did not name the missing version. Output was:\n${_output}"
    )
endif()

# The same file feeds the CI build matrix, so the emitted JSON is parsed back
# with CMake's own JSON reader. A hand-built string that only looks like JSON
# would otherwise turn into a broken matrix on GitHub rather than a failure
# here.
set(_matrix_file "${CMAKE_CURRENT_BINARY_DIR}/python_versions_for_test.json")
file(REMOVE "${_matrix_file}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DMAIS_PYTHON_MATRIX_OUT=${_matrix_file}"
        -P "${MAIS_SOURCE_DIR}/cmake/PythonSupport.cmake"
    RESULT_VARIABLE _result
    ERROR_VARIABLE _output
)
if(NOT "${_result}" EQUAL 0)
    message(FATAL_ERROR
        "Emitting the CI matrix failed. Output was:\n${_output}"
    )
endif()

file(READ "${_matrix_file}" _matrix_json)
string(JSON _matrix_length LENGTH "${_matrix_json}")
list(LENGTH MAIS_PYTHON_VERSIONS _declared_length)
if(NOT _matrix_length EQUAL _declared_length)
    message(FATAL_ERROR
        "The emitted matrix has ${_matrix_length} entries but "
        "${_declared_length} versions are declared. Content was: "
        "${_matrix_json}"
    )
endif()

set(_index 0)
foreach(_expected IN LISTS MAIS_PYTHON_VERSIONS)
    string(JSON _actual GET "${_matrix_json}" ${_index})
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR
            "Matrix entry ${_index} is \"${_actual}\" but "
            "cmake/PythonSupport.cmake declares \"${_expected}\". Content was: "
            "${_matrix_json}"
        )
    endif()
    math(EXPR _index "${_index} + 1")
endforeach()
