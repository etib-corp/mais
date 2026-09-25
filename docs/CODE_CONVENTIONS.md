# Code Conventions

This document defines coding standards for maïs. The goal is consistent,
readable, and maintainable code.

## General Rules

- Use clear and descriptive names.
- Indentation is tabs and the line limit is 80 columns; both are enforced by
  `.clang-format`. Run `scripts/run-clang-format.sh` before opening a PR.
- Prefer small, focused functions over large monolithic blocks.
- Avoid duplicated logic (DRY principle).
- Add comments only when intent is not obvious.
- Validate inputs and handle errors explicitly.

## C++ Conventions

- Use `PascalCase` for classes and `camelCase` for functions and variables.
- Prefix member fields with `_`.
- Use `nullptr` instead of `NULL` or `0`.
- Prefer RAII and smart pointers (`std::unique_ptr`, `std::shared_ptr`).
- Apply `const` correctness whenever possible.
- Prefer `enum class` over unscoped enums.
- Use `std::string_view` for read-only string parameters.
- Keep pybind11 out of public headers. Hide implementation details behind a
  forward-declared implementation type, as `mais::ScriptRuntime` does.

## Class Layout

In class declarations, keep sections in this order:

1. `public`
2. `protected`
3. `private`

Within each section, keep this order:

1. Types (`struct`, `enum`, aliases)
2. Constructors and special members
3. Methods
4. Fields

## Performance

- Do not build Python objects per frame. Every `ScriptRuntime::call()` crosses
  into the interpreter, so pass scalars and let scripts read long-lived host
  objects from their module instead of rebuilding them.
- Prefer `callOptional()` for hooks. It keeps the host loop free of "does the
  script implement this?" checks and short-circuits absent optional hooks.
- Reuse what the runtime already prepares: `loadModule()` performs an import,
  and the `traceback` module is imported once in `initialize()` so error
  reporting never imports while an exception is pending.
- Measure before optimizing. `benchmarks/` measures per-call overhead with
  Google Benchmark (`-DBUILD_BENCHMARKS=ON`).

## Scripting boundary

- Keep the Python-facing surface small and deliberate: bind the operations
  scripts actually need rather than whole engine or application classes.
- Never let Python take ownership of a host object. Attach pointers with
  `pybind11::return_value_policy::reference` (or `reference_internal`); the
  interpreter must be stopped before those objects are destroyed.
- Hook names are host-owned contracts. Document the point in the host loop at
  which each one runs.

## Checklist Before Opening A PR

- Naming follows conventions.
- Class and method order is consistent.
- No obvious duplication.
- Error handling is present where needed.
- Formatting is clean and consistent.
