# Probe used by test_python_support.cmake. Runs in CMake script mode with a
# synthetic Python_VERSION, so the guard can be exercised without installing
# the interpreter it pretends to have found.
include("${MAIS_SOURCE_DIR}/cmake/PythonSupport.cmake")
mais_require_supported_python()
