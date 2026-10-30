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

list(GET MAIS_PYTHON_VERSIONS 0 MAIS_PYTHON_MIN)
list(LENGTH MAIS_PYTHON_VERSIONS _mais_python_count)
math(EXPR _mais_python_last "${_mais_python_count} - 1")
list(GET MAIS_PYTHON_VERSIONS ${_mais_python_last} MAIS_PYTHON_MAX)
unset(_mais_python_count)
unset(_mais_python_last)

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
    if(NOT Python_VERSION)
        return()
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
