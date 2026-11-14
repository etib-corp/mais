---
name: add-source-file
description: Add, rename or remove a header, source, test, test script or example in mais. Use whenever a new file is created, because source and test lists are explicit and a forgotten entry breaks the build or silently skips tests.
---

# Add a file to mais

Nothing is globbed. Every file is listed by hand.

## Library code

1. Header: `headers/mais/<Name>.hpp`. No pybind11 include: hide the
   implementation (`ScriptRuntime` uses a private `Impl`).
2. Source: `sources/<Name>.cpp`; add it to `MAIS_SOURCES` in
   `cmake/Sources.cmake`.
3. Follow a neighbouring file for the license header, Doxygen comments and
   class layout. Report failures as `mais::Error` or `mais::Result`, with the
   Python traceback when there is one.

## Test

1. `tests/sources/test_<name>.cpp`; add it to `TEST_SOURCES` in
   `tests/CMakeLists.txt`. An unlisted test is not compiled, and nothing
   warns about it.
2. Python scripts used by tests go in `tests/sources/scripts/`
   (`MAIS_TEST_SCRIPT_DIR` points there). Fake host services are in
   `tests/headers/FakeHost.hpp`.
3. Cover lifecycle edges with every API change: before `initialize()`, after
   `shutdown()`, and a call from another thread (`docs/THREADING.md`).

## Example host

Add a directory under `examples/` with its own `CMakeLists.txt` and
`scripts/`, list it in `examples/CMakeLists.txt` and describe it in
`examples/README.md`. An example is a standalone host with a stub engine.

## Supported Python versions

`cmake/PythonSupport.cmake` is the single declaration: configure and the CI
matrix both read it. Add a version only in a pull request where CI passes for
it on all three systems.

## Finish

Follow the `build-and-test` and `check-boundaries` skills. Update `docs/` and
`CHANGELOG.md` when public behaviour changes.
