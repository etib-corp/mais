#!/usr/bin/env bash
#
# One entry point for the gate AGENTS.md describes: maïs stays generic.
#
#   1. headers/ and sources/ include only the C++ standard library, CPython,
#      pybind11, and maïs's own headers.
#   2. the mais target links only pybind11::embed and the Python runtime that
#      pybind11 carries, checked by configuring the library on its own with no
#      consumer present.
#
# The link half also runs during every configure (MAIS_CHECK_BOUNDARIES is ON by
# default), so a consumer that adds a dependency to the target after
# add_subdirectory() fails its own build. This script adds the standalone
# configure and the include scan, and is what CI runs.
#
# --verify additionally proves the gate is not vacuous: it injects a forbidden
# include, a consumer that pushes a dependency into the target, and a dependency
# hidden behind the approved pybind11 target, and fails if any of them passes.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build/boundaries"
python_bin=""
verify_gate=OFF
configure_arguments=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] [-- <cmake arguments>]

Checks the architectural boundary of maïs (see AGENTS.md).

Options:
  --build-dir <dir>  where to configure the standalone check
                     (default: build/boundaries)
  --python <path>    interpreter used for the include scan
                     (default: python3, then python)
  --verify           also inject a forbidden include and consumer
                     dependencies, and fail unless each is rejected
  -h, --help         show this help

Arguments after -- are passed to the configure step, for example
  -- -DPython_EXECUTABLE=/usr/bin/python3
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="${2:?--build-dir needs a value}"
            shift 2
            ;;
        --python)
            python_bin="${2:?--python needs a value}"
            shift 2
            ;;
        --verify)
            verify_gate=ON
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        --)
            shift
            configure_arguments=("$@")
            break
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$python_bin" ]]; then
    for candidate in python3 python; do
        if command -v "$candidate" > /dev/null 2>&1; then
            python_bin="$candidate"
            break
        fi
    done
fi
if [[ -z "$python_bin" ]]; then
    echo "error: no python interpreter found" >&2
    echo "       maïs embeds Python, so one is required" >&2
    exit 1
fi
if ! command -v cmake > /dev/null 2>&1; then
    echo "error: cmake was not found on PATH" >&2
    exit 1
fi

# Proves that the gate rejects what it is meant to reject: a gate that silently
# accepts everything would otherwise look identical to one that passes. The
# probes are temporary; the include probe is removed on exit even when the run
# is interrupted.
verify() {
    local failures=0

    echo "== verify: a forbidden include is rejected =="
    local include_probe="$repo_root/sources/__boundary_probe.cpp"
    printf '#include <vulkan/vulkan.h>\n' > "$include_probe"
    if "$python_bin" "$repo_root/scripts/check-boundaries-includes.py" \
        --root "$repo_root" --quiet > /dev/null 2>&1; then
        echo "  FAIL: a forbidden include was accepted" >&2
        failures=1
    else
        echo "  ok: <vulkan/vulkan.h> was rejected"
    fi
    rm -f "$include_probe"

    local probe_root="$build_dir/boundary-consumer"
    mkdir -p "$probe_root"
    # Configuring maïs from a consumer is the case a library-subdirectory
    # configure can get wrong, so both probes are real consumers.
    cat > "$probe_root/prelude.cmake" <<EOF
cmake_minimum_required(VERSION 3.20)
project(boundary_consumer LANGUAGES CXX)
add_subdirectory("$repo_root" mais)
EOF

    echo "== verify: a dependency pushed into the target is rejected =="
    mkdir -p "$probe_root/direct"
    cat > "$probe_root/direct/CMakeLists.txt" <<'EOF'
include("../prelude.cmake")

add_library(fake_engine INTERFACE)
target_link_libraries(mais PRIVATE fake_engine)
EOF
    if check_link_probe "$probe_root/direct"; then
        echo "  ok: a dependency pushed into the target was rejected"
    else
        failures=1
    fi

    echo "== verify: a dependency behind the approved target is rejected =="
    mkdir -p "$probe_root/transitive"
    cat > "$probe_root/transitive/CMakeLists.txt" <<'EOF'
include("../prelude.cmake")

# The approved target itself reaching an engine must fail too, which is the
# path that only the transitive walk can see.
add_library(fake_engine INTERFACE)
set_property(TARGET pybind11::embed APPEND
    PROPERTY INTERFACE_LINK_LIBRARIES fake_engine)
EOF
    if check_link_probe "$probe_root/transitive"; then
        echo "  ok: a dependency behind the approved target was rejected"
    else
        failures=1
    fi

    return "$failures"
}

# Configures one probe project and reports whether the boundary check rejected
# it. Returns success when the gate fired, and records a failure otherwise.
check_link_probe() {
    local project_dir="$1"
    local log="$project_dir/configure.log"

    # Reuse the pybind11 checkout the standalone configure already fetched so
    # that the probes stay offline and fast.
    local reuse=()
    if [[ -d "$build_dir/_deps/pybind11-src" ]]; then
        reuse=(
            -DFETCHCONTENT_SOURCE_DIR_PYBIND11="$build_dir/_deps/pybind11-src"
        )
    fi

    if cmake -S "$project_dir" -B "$project_dir/build" \
        -DMAIS_CHECK_BOUNDARIES=ON \
        -DBUILD_TESTING=OFF \
        -DBUILD_DOCS=OFF \
        ${reuse[@]+"${reuse[@]}"} > "$log" 2>&1; then
        echo "  FAIL: the violation was accepted" >&2
        return 1
    fi
    if ! grep -q "boundary check failed" "$log"; then
        echo "  FAIL: configure failed without the boundary check reporting" >&2
        sed -n '/Error/,+4p' "$log" >&2
        return 1
    fi
    return 0
}

if [[ $verify_gate == ON ]]; then
    trap 'rm -f "$repo_root/sources/__boundary_probe.cpp"' EXIT
fi

status=0

echo "== includes (headers/, sources/) =="
# Both halves run even when the first fails, so a single run reports everything
# that crossed the boundary.
if ! "$python_bin" "$repo_root/scripts/check-boundaries-includes.py" \
    --root "$repo_root"; then
    status=1
fi

echo "== link dependencies (standalone configure) =="
# Tests, examples, benchmarks, and documentation are off: the link interface is
# already known after configure, and skipping the rest keeps the gate to a few
# seconds. The condition below keeps the array expansion safe on bash 3.2, which
# treats "${empty[@]}" as an unset variable under `set -u`.
if ! cmake -S "$repo_root" -B "$build_dir" \
    -DMAIS_CHECK_BOUNDARIES=ON \
    -DBUILD_TESTING=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_DOCS=OFF \
    ${configure_arguments[@]+"${configure_arguments[@]}"}; then
    status=1
fi

if [[ $status -ne 0 ]]; then
    echo "boundaries: FAILED, see the violations above" >&2
    exit "$status"
fi

echo "boundaries: maïs includes only approved dependencies and links only"
echo "            pybind11 and the Python runtime"

if [[ $verify_gate == ON ]]; then
    if ! verify; then
        echo "boundaries: the gate did not reject what it must reject" >&2
        exit 1
    fi
    echo "boundaries: the gate rejects a forbidden include, a dependency"
    echo "            pushed into the target, and one hidden behind pybind11"
fi
