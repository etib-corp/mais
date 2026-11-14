---
name: check-boundaries
description: Check that mais stays a generic scripting library with no engine, renderer or UI dependency, and fix a failing boundary check. Use after adding an include, a dependency or a CMake link, and before every commit.
---

# Keep mais generic

mais may reach the C++ standard library, CPython, pybind11 and its own
headers. Nothing else. This is checked, not only documented.

```sh
./scripts/check-boundaries.sh            # the gate; CI runs it
./scripts/check-boundaries.sh --verify   # also proves the gate rejects violations
```

The gate has two halves:

1. **Includes**: `headers/` and `sources/` are scanned for any include
   outside the approved set.
2. **Links**: the library is configured on its own and the `mais` target's
   transitive link interface must stop at `pybind11::embed` and the Python
   runtime. This half also runs in every configure
   (`MAIS_CHECK_BOUNDARIES`, on by default), so a consumer that adds a
   dependency to the target fails its own build.

Both halves are default-deny: a new dependency fails without the check
knowing its name.

## When it fails

Do not add the dependency to the approved set to make it pass. First ask
where the code belongs:

- Engine, window, renderer or frame-loop code belongs in the host or in a
  separate adapter that depends on mais, never the reverse.
- UI code belongs in the application.
- A truly generic dependency is a compatibility decision: explain the impact
  and get maintainer approval before touching
  `cmake/CheckBoundaries.cmake` or `scripts/check-boundaries-includes.py`.

Also keep these, which the gate does not fully cover:

- Public headers in `headers/mais/` do not include pybind11
  (`tests/sources/test_public_headers.cpp`). pybind11 stays `PRIVATE`.
- `examples/` and `tests/` use fake or stub hosts, never guillaume or evan.

Run `--verify` after changing the gate itself. `docs/ARCHITECTURE.md` and
`docs/HOST_INTEGRATION.md` state what belongs where.
