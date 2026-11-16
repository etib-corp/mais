---
name: build-and-test
description: Configure, build, test and format mais. Use after any change to headers/, sources/, tests/, examples/ or the CMake files, and whenever asked to build, test or check the library.
---

# Build and test mais

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DPython_EXECUTABLE="$(command -v python3)"
cmake --build build -j 2>&1 | grep -E "error|Error [0-9]|Built target"
ctest --test-dir build --output-on-failure
```

- Python must come with its development files (headers and the embeddable
  library). The interpreter must be in the range declared in
  `cmake/PythonSupport.cmake`; configure rejects anything else by name.
- `-DPython_EXECUTABLE` selects the interpreter. pybind11 owns Python
  discovery: never add a second `find_package(Python...)`.
- pybind11 is fetched when it is not installed (network on first configure).
  An installed pybind11 of another major version is rejected.
- No Vulkan, window or engine is needed. If a build asks for one, a boundary
  was broken: run the `check-boundaries` skill.
- `-DBUILD_EXAMPLES=ON` builds the example hosts. With tests on, they are
  registered in CTest and are the acceptance tests of the hook behaviour.
- `-DDEV_MODE=ON` compiles with warnings as errors. Use it before handing a
  change back.

## Always run with the tests

```sh
./scripts/check-boundaries.sh
./scripts/run-clang-format.sh
```

Use the clang-format version CI installs (`.github/workflows/format.yml`).

## Report

State the Python version used, the test count, whether the examples were
built, and the result of the boundary check. CI runs every supported Python
version on three systems; a local run covers one.
