# Supported Python range for maïs.
#
# This list is the single declaration of the interpreter versions maïs is
# built and tested against. .github/workflows/build.yml derives its build
# matrix from this file, and the guard below rejects anything outside it, so
# CI and CMake cannot disagree about what is supported.
#
# Entries are `major.minor`. Only add a version once it has passed the CI
# matrix on Linux, macOS, and Windows in the pull request that adds it.
set(MAIS_PYTHON_VERSIONS 3.9 3.10 3.11 3.12 3.13 3.14)

# Computes the floor and ceiling of a version list.
#
# The extremes are found by comparison, not taken from the first and last
# entries, so the order of MAIS_PYTHON_VERSIONS cannot change what configure
# accepts: appending 3.8 would otherwise make 3.8 the ceiling and reject every
# supported version.
function(mais_python_supported_range versions out_min out_max)
    set(_mais_python_min "")
    set(_mais_python_max "")

    foreach(_mais_python_version IN LISTS versions)
        if(_mais_python_min STREQUAL "")
            set(_mais_python_min "${_mais_python_version}")
        elseif(_mais_python_version VERSION_LESS _mais_python_min)
            set(_mais_python_min "${_mais_python_version}")
        endif()

        if(_mais_python_max STREQUAL "")
            set(_mais_python_max "${_mais_python_version}")
        elseif(_mais_python_version VERSION_GREATER _mais_python_max)
            set(_mais_python_max "${_mais_python_version}")
        endif()
    endforeach()

    # An empty list would leave a range of two empty strings, which compares as
    # 0 and accepts every interpreter, so it is a configuration error rather
    # than an unsupported range.
    if(_mais_python_min STREQUAL "")
        message(FATAL_ERROR
            "mais declares no supported Python versions in "
            "cmake/PythonSupport.cmake. Add at least one, or the "
            "supported-range guard would accept every interpreter."
        )
    endif()

    set(${out_min} "${_mais_python_min}" PARENT_SCOPE)
    set(${out_max} "${_mais_python_max}" PARENT_SCOPE)
endfunction()

mais_python_supported_range(
    "${MAIS_PYTHON_VERSIONS}" MAIS_PYTHON_MIN MAIS_PYTHON_MAX
)

# Script mode (`cmake -P cmake/PythonSupport.cmake`): write the list out as a
# JSON array so CI can turn it into a build matrix without repeating it.
if(MAIS_PYTHON_MATRIX_OUT)
    string(REPLACE ";" "\", \"" _mais_python_json "${MAIS_PYTHON_VERSIONS}")
    file(WRITE "${MAIS_PYTHON_MATRIX_OUT}" "[\"${_mais_python_json}\"]")
endif()

# Fails configuration when the resolved interpreter is outside the list.
#
# Called once pybind11 has run Python discovery. Without this check an
# unsupported interpreter surfaces much later, as a compilation error inside
# pybind11 or as a runtime failure, naming neither the range nor the
# interpreter that was actually selected.
function(mais_require_supported_python)
    # pybind11 populates Python_VERSION during discovery, and configure calls
    # this immediately afterwards. An absent version therefore means discovery
    # changed shape; failing loudly keeps the range enforced, whereas returning
    # would silently accept every interpreter with no test noticing.
    if(NOT Python_VERSION)
        message(FATAL_ERROR
            "mais cannot check the supported Python range: the resolved "
            "interpreter reported no version, so Python_VERSION is empty. "
            "Python discovery ran without populating it; fix the discovery "
            "wiring in CMakeLists.txt rather than skipping the check."
        )
    endif()

    string(REGEX MATCH "^[0-9]+\\.[0-9]+" _mais_python_found "${Python_VERSION}")
    if(_mais_python_found VERSION_GREATER_EQUAL MAIS_PYTHON_MIN
       AND _mais_python_found VERSION_LESS_EQUAL MAIS_PYTHON_MAX)
        return()
    endif()

    string(REPLACE ";" ", " _mais_python_list "${MAIS_PYTHON_VERSIONS}")
    set(_mais_python_selected "")
    if(Python_EXECUTABLE)
        set(_mais_python_selected " (${Python_EXECUTABLE})")
    endif()

    message(FATAL_ERROR
        "maïs supports Python ${MAIS_PYTHON_MIN} through ${MAIS_PYTHON_MAX}, "
        "but the selected interpreter is Python ${Python_VERSION}"
        "${_mais_python_selected}.\n"
        "Supported versions: ${_mais_python_list}.\n"
        "Select a supported interpreter with "
        "-DPython_EXECUTABLE=/path/to/python3. If the version you tried is "
        "meant to be supported, add it to cmake/PythonSupport.cmake once it "
        "passes on Linux, macOS, and Windows in CI."
    )
endfunction()
