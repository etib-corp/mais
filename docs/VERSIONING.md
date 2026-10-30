# Versioning & Support Policy

This document describes how maïs is versioned and supported.

## Versioning

Mais follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(`MAJOR.MINOR.PATCH`):

- **MAJOR** — incompatible API changes.
- **MINOR** — backwards-compatible feature additions.
- **PATCH** — backwards-compatible bug fixes.

The current version is declared in `CMakeLists.txt` and mirrored in
`CHANGELOG.md`.

## Long-Term Support (LTS)

- The **1.x** line is the current supported series.
- Bug fixes and security patches are backported to the latest 1.x release.
- New features land in the next minor release.

## Deprecation Window

- Deprecated APIs are documented in the API reference and the `CHANGELOG.md`.
- A deprecated API is removed **no earlier than two minor releases** after it
  is marked deprecated.
- Each release ships a **migration guide** that documents breaking changes and
  how to update.

## Platform & Toolchain Support

A support claim is made only for what the CI matrix actually builds and tests.
Anything else is unvalidated and therefore unsupported.

| Platform | CI image | Python |
| --- | --- | --- |
| Linux | `ubuntu-24.04` | 3.9 – 3.14 |
| macOS | `macos-latest` | 3.9 – 3.14 |
| Windows | `windows-latest` | 3.9 – 3.14 |

`cmake/PythonSupport.cmake` is the authoritative declaration of that range. It is
the only place the range appears in the build: configuration rejects an
interpreter outside it, and `.github/workflows/build.yml` generates its matrix
from the same file, so the tested versions and the accepted versions cannot
drift apart. The table in `README.md` states the same matrix for users and must
be updated with it.

Extending the range:

1. Add the version to `cmake/PythonSupport.cmake`.
2. Open a pull request and let the generated matrix pass on every platform.
3. Update the table here and in `README.md` in the same pull request.

Dropping a version is a compatibility change and follows the deprecation window
above.

## Communication

- Every release is announced in `CHANGELOG.md`.
- Breaking changes are always accompanied by a migration guide entry.
- Security issues are handled per `SECURITY.md`.

## Release Cadence

Releases are cut when the maintainers determine the `main` branch is stable
and the release checklist is complete. There is no fixed calendar schedule.
