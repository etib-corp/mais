#!/usr/bin/env python3
"""Reject includes that cross maïs's dependency boundary.

`headers/` and `sources/` are the library itself, so they may include only the
C++ standard library, CPython's C API, pybind11, and maïs's own headers.
Everything else -- an engine, a renderer, a UI toolkit, a consumer's headers --
belongs to the consumer or to an adapter that depends on maïs.

The check is a default-deny list of approved owners rather than a list of
forbidden projects, so the next engine to try moving in is rejected without
this file having to learn its name. Only real `#include` directives are read:
comments and documentation may discuss `evan` and `guillaume` freely.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Directories that belong to the library. Tests, examples, and benchmarks are
# hosts on purpose and may include whatever their host needs.
SCANNED_DIRECTORIES = ("headers", "sources")
SOURCE_SUFFIXES = frozenset(
    {".hpp", ".h", ".hh", ".inl", ".cpp", ".cc", ".cxx"}
)

# C++20 standard library headers plus the C standard library headers a C++
# translation unit may legitimately include.
STANDARD_HEADERS = frozenset(
    """
    algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv
    cfloat charconv chrono cinttypes climits clocale cmath codecvt compare
    complex concepts condition_variable coroutine csetjmp csignal cstdarg
    cstddef cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype
    deque exception execution expected filesystem format forward_list fstream
    functional future initializer_list iomanip ios iosfwd iostream istream
    iterator latch limits list locale map mdspan memory memory_resource mutex
    new numbers numeric optional ostream print queue random ranges ratio regex
    scoped_allocator semaphore set shared_mutex source_location span spanstream
    sstream stack stacktrace stdexcept stdfloat stop_token streambuf string
    string_view strstream syncstream system_error thread tuple type_traits
    typeindex typeinfo unordered_map unordered_set utility valarray variant
    vector version
    assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h iso646.h
    limits.h locale.h math.h setjmp.h signal.h stdalign.h stdarg.h stdatomic.h
    stdbit.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h
    string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h
    """.split()
)

# CPython's public C API headers that embedders include directly from the
# interpreter's include directory.
PYTHON_HEADERS = frozenset(
    """
    Python.h abstract.h bltinmodule.h boolobject.h bytearrayobject.h
    bytesobject.h ceval.h code.h compile.h complexobject.h datetime.h
    descrobject.h dictobject.h dynamic_annotations.h enumobject.h errcode.h
    exports.h fileobject.h fileutils.h floatobject.h frameobject.h
    genericaliasobject.h import.h interpreteridobject.h intrcheck.h iterobject.h
    listobject.h longobject.h marshal.h memoryobject.h methodobject.h
    modsupport.h moduleobject.h object.h objimpl.h opcode.h osdefs.h osmodule.h
    patchlevel.h pyarena.h pyatomic.h pycapsule.h pyconfig.h pydtrace.h
    pyerrors.h pyexpat.h pyframe.h pyhash.h pylifecycle.h pymacconfig.h
    pymacro.h pymath.h pymem.h pyport.h pystate.h pystrcmp.h pystrtod.h
    pythonrun.h pythread.h pytypedefs.h rangeobject.h setobject.h sliceobject.h
    structmember.h structseq.h sysmodule.h traceback.h tracemalloc.h
    tupleobject.h typeslots.h unicodeobject.h warnings.h weakrefobject.h
    """.split()
)

# Include prefixes owned by the approved dependencies. `mais/` is the library
# itself, always addressed relative to headers/ so consumers can include it the
# same way.
APPROVED_PREFIXES = ("mais/", "pybind11/", "cpython/")

INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*(?:<([^>]+)>|"([^"]+)")')
INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\b")


def _blank(text: str) -> str:
    """Return `text` with everything but its newlines replaced by spaces."""
    return "".join("\n" if character == "\n" else " " for character in text)


def _raw_string_content_start(text: str, index: int) -> int | None:
    """Return where a raw string's delimiter starts, or None if there is none.

    A raw string is `R"delim(...)delim"`, optionally prefixed with an encoding
    (`u8`, `u`, `U`, `L`). The prefix is allowed only at an identifier boundary.
    """
    if text[index : index + 2] != 'R"':
        return None
    for prefix in ("u8", "u", "U", "L", ""):
        begin = index - len(prefix)
        if begin < 0 or not text.startswith(prefix, begin):
            continue
        if begin == 0:
            return index + 2
        previous = text[begin - 1]
        if not (previous.isalnum() or previous == "_"):
            return index + 2
    return None


def _raw_string_end(text: str, content_start: int) -> int:
    """Return the index just past the raw string literal, or `len(text)`."""
    # A delimiter is at most 16 characters and may not contain whitespace,
    # parentheses, or a backslash.
    parenthesis = text.find("(", content_start, content_start + 17)
    if parenthesis == -1:
        return content_start
    delimiter = text[content_start:parenthesis]
    if any(character in " ()\\\t\v\f\n" for character in delimiter):
        return content_start
    terminator = ")" + delimiter + '"'
    end = text.find(terminator, parenthesis + 1)
    return content_start if end == -1 else end + len(terminator)


def mask_comments(text: str) -> str:
    """Blank out comments while keeping line structure and string literals.

    Include directives are only meaningful outside comments, so a documentation
    comment that mentions an engine header must not be counted. String literals
    are preserved because a quoted include *is* a string literal, but their
    contents are skipped so that a `/*` inside one cannot open a comment. Raw
    string literals are skipped whole, since their contents may hold anything.
    """
    output: list[str] = []
    state = "code"
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        following = text[index + 1] if index + 1 < length else ""

        if state == "code":
            if character == "R":
                content_start = _raw_string_content_start(text, index)
                if content_start is not None:
                    end = _raw_string_end(text, content_start)
                    # Its contents are blanked rather than kept: a raw string
                    # may span lines and hold something that looks like an
                    # include directive.
                    output.append(_blank(text[index:end]))
                    index = end
                    continue
            if character == "/" and following == "/":
                state = "line_comment"
                output.append("  ")
                index += 2
                continue
            if character == "/" and following == "*":
                state = "block_comment"
                output.append("  ")
                index += 2
                continue
            if character in "\"'":
                state = "string" if character == '"' else "character"
            output.append(character)
            index += 1
            continue

        if state == "line_comment":
            if character == "\n":
                state = "code"
                output.append("\n")
            else:
                output.append("\t" if character == "\t" else " ")
            index += 1
            continue

        if state == "block_comment":
            if character == "*" and following == "/":
                state = "code"
                output.append("  ")
                index += 2
                continue
            output.append("\n" if character == "\n" else " ")
            index += 1
            continue

        # Inside a string or character literal: keep the text, honor escapes,
        # and leave the literal on its closing quote.
        if character == "\\" and following:
            output.append(character)
            output.append(following)
            index += 2
            continue
        if (state == "string" and character == '"') or (
            state == "character" and character == "'"
        ):
            state = "code"
        output.append(character)
        index += 1

    return "".join(output)


# A rejected include: file line, include text, and the reason it is rejected.
Violation = tuple[int, str, str]


def classify(include: str, including_file: Path, root: Path) -> str | None:
    """Return why an include is rejected, or None when it is approved."""
    normalized = include.strip().replace("\\", "/")

    if normalized.startswith(APPROVED_PREFIXES):
        return None
    if normalized in STANDARD_HEADERS or normalized in PYTHON_HEADERS:
        return None

    # A quoted include may address a sibling file directly; that is still part
    # of the library when it resolves inside the including directory or inside
    # headers/.
    candidates = (
        including_file.parent / normalized,
        root / "headers" / normalized,
    )
    for candidate in candidates:
        if not candidate.is_file():
            continue
        # A relative include may point anywhere on disk; only a file that
        # resolves inside the repository is part of the library.
        try:
            candidate.resolve().relative_to(root)
        except ValueError:
            continue
        return None

    return (
        f"'{normalized}' is neither a standard library, CPython, pybind11, "
        "nor maïs header"
    )


def iter_directives(text: str) -> list[tuple[int, str, str | None]]:
    """Return (line, directive, include path) for every #include directive.

    The include path is None when the directive is not a literal `<...>` or
    `"..."` form, which is reported as a violation rather than ignored.
    """
    directives: list[tuple[int, str, str | None]] = []
    for number, line in enumerate(mask_comments(text).splitlines(), start=1):
        match = INCLUDE_PATTERN.match(line)
        if match:
            include = match.group(1) or match.group(2)
            directives.append((number, line.strip(), include))
        elif INCLUDE_DIRECTIVE.match(line):
            directives.append((number, line.strip(), None))
    return directives


def scan_text(text: str, including_file: Path, root: Path) -> list[Violation]:
    """Return every include that crosses the boundary, with its reason."""
    violations: list[Violation] = []
    for number, directive, include in iter_directives(text):
        if include is None:
            violations.append(
                (number, directive, "is not a literal <...> or \"...\" include")
            )
            continue
        reason = classify(include, including_file, root)
        if reason:
            violations.append((number, include, reason))
    return violations


SELF_TEST_CASES = (
    # Approved dependencies.
    ("#include <string>", 0),
    ("#include <pybind11/embed.h>\n#include <pybind11/pybind11.h>", 0),
    ("#include <Python.h>\n#include <frameobject.h>\n#include <pythread.h>", 0),
    ('#include "mais/Error.hpp"\n#include "mais/ScriptRuntime.hpp"', 0),
    # Prose must not trip the check: comments are blanked before matching.
    ("// Bindings would go here: #include <evan/Engine.hpp>", 0),
    ("/*\n * evan and guillaume stay outside.\n */\n#include <string>", 0),
    ('const char *note = "never #include <vulkan/vulkan.h> here";', 0),
    # A raw string may hold anything, including comment markers and directives.
    ('auto s = R"(\n// #include <vulkan/vulkan.h>\n)";\n#include <string>', 0),
    ('auto s = u8R"t(\n#include <GLFW/glfw3.h>\n)t";\n#include <string>', 0),
    # Everything else is a violation, including names this file never mentions
    # by project.
    ("#include <vulkan/vulkan.h>", 1),
    ("#include <GLFW/glfw3.h>", 1),
    ("#include <imgui.h>", 1),
    ("#include <glm/glm.hpp>", 1),
    ("#include <stb_image.h>", 1),
    ('#include "evan/Engine.hpp"', 1),
    ('#include "guillaume/UI.hpp"', 1),
)


def run_self_test(root: Path) -> list[str]:
    """Prove the checker still rejects and still approves the obvious cases."""
    failures: list[str] = []
    including_file = root / "sources" / "self_test.cpp"
    for source, expected in SELF_TEST_CASES:
        found = scan_text(source, including_file, root)
        if len(found) != expected:
            failures.append(
                f"self-test expected {expected} violation(s) for {source!r}, "
                f"found {len(found)}"
            )
    return failures


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Reject includes outside maïs's approved dependency set.",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root (defaults to the parent of this script)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="print only failures",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="only run the checker's own cases, without scanning the tree",
    )
    arguments = parser.parse_args(argv)

    root = arguments.root.resolve()
    if not (root / "headers").is_dir() and not (root / "sources").is_dir():
        print(
            f"error: {root} does not look like the maïs repository",
            file=sys.stderr,
        )
        return 2

    self_test_failures = run_self_test(root)
    if self_test_failures:
        print(
            "boundary check failed: the include scanner is unsound",
            file=sys.stderr,
        )
        for failure in self_test_failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    if arguments.self_test:
        if not arguments.quiet:
            print(
                "includes: scanner self-test passed "
                f"({len(SELF_TEST_CASES)} cases)"
            )
        return 0

    files = 0
    includes = 0
    violations: list[tuple[Path, int, str, str]] = []
    for directory in SCANNED_DIRECTORIES:
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            files += 1
            text = path.read_text(encoding="utf-8", errors="replace")
            includes += len(iter_directives(text))
            for number, include, reason in scan_text(text, path, root):
                violations.append((path, number, include, reason))

    if violations:
        print(
            f"boundary check failed: {len(violations)} include(s) cross "
            "maïs's dependency boundary",
            file=sys.stderr,
        )
        for path, number, include, reason in violations:
            location = path.relative_to(root)
            print(
                f"  {location}:{number}: {include} -- {reason}",
                file=sys.stderr,
            )
        print(
            "\nheaders/ and sources/ may include the C++ standard library, "
            "CPython, pybind11, and maïs's own headers. Engine, renderer, UI, "
            "and consumer dependencies belong to the consumer or to an adapter "
            "that depends on maïs. If a dependency has been explicitly "
            "approved, add it to scripts/check-boundaries-includes.py and "
            "AGENTS.md together.",
            file=sys.stderr,
        )
        return 1

    if not arguments.quiet:
        print(
            f"includes: {includes} directive(s) in {files} file(s) stay inside "
            "the standard library, CPython, pybind11, and maïs"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
