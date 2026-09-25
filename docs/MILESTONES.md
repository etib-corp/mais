# maïs Milestones

This document turns [`ROADMAP.md`](ROADMAP.md) into shippable increments.

| Document | Answers |
| --- | --- |
| [`ROADMAP.md`](ROADMAP.md) | **Why** and **what**, with owners and open questions |
| **`MILESTONES.md`** | **When**: the ordered increments, their exit criteria, and their release |
| [`issues/`](issues/) | **How**: one reviewable spec per unit of work |

## How the project is built iteratively

A **milestone** is one shippable increment: it ends with a green build, a release
per [`VERSIONING.md`](VERSIONING.md), and a `CHANGELOG.md` entry. A milestone is
not a phase to plan for months — it is the next thing that makes the library
measurably more useful to the XR/UI consumer.

An **issue** is one pull request. It follows the repository's commit
conventions, keeps the verification gates green, and states how it will be
verified before it is started.

### Definition of ready

An issue may be started when it has an owner, a stated scope, and acceptance
criteria that a reviewer can check. Items blocked on an open question in
`ROADMAP.md` stay unstarted until that question is answered.

### Definition of done

- [ ] Acceptance criteria in the issue are met.
- [ ] New behaviour is covered by a test in `tests/`, or by the example host.
- [ ] Public API changes are in `CHANGELOG.md` under `[Unreleased]`.
- [ ] `scripts/run-clang-format.sh` leaves no diff.
- [ ] `ctest --test-dir build --output-on-failure` passes, including the
      standalone-host integration test.
- [ ] `AGENTS.md`'s change checklist still holds — in particular, no consumer,
      engine, or UI dependency entered maïs.

### Verification gates

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DPython_EXECUTABLE="$(command -v python3)"
cmake --build build
ctest --test-dir build --output-on-failure
bash scripts/run-clang-format.sh
```

## Milestone map

| Milestone | Goal | Release | Issues | Status |
| --- | --- | --- | --- | --- |
| **M0** | Generic runtime a host can embed | 1.0.0 | delivered | Done |
| **M1** | Invocation surface rich enough for a real XR/UI host | 1.1.0 | 8 | Next |
| **M2** | Prove and document the consumer integration path | 1.2.0 | 3 | Next |
| **M3** | Reload scripts without restarting the headset app | 1.3.0 | 4 | Later |
| **M4** | Survive long XR sessions | 1.4.0 | 5 | Later |
| **M5** | Breadth, only if a second real consumer justifies it | 2.0.0 | 0 | Conditional |

M0 is complete: interpreter lifecycle, host binding registration, script
loading, invocation, traceback reporting, the standalone host example, the
installed package, and 32 passing tests. Everything below builds on that.

## Tracker

The milestones and issues live on GitHub. `docs/issues/` stays the reviewable
spec, and [`scripts/create-roadmap-issues.sh`](../scripts/create-roadmap-issues.sh)
recreates the tracker objects idempotently.

| Milestone | Tracker | Issues |
| --- | --- | --- |
| M1 | [milestone/1](https://github.com/etib-corp/mais/milestone/1) | #1 - #8 |
| M2 | [milestone/2](https://github.com/etib-corp/mais/milestone/2) | #9 - #11 |
| M3 | [milestone/3](https://github.com/etib-corp/mais/milestone/3) | #12 - #15 |
| M4 | [milestone/4](https://github.com/etib-corp/mais/milestone/4) | #16 - #20 |

Issues are numbered in backlog order, so MAIS-1.4 is #4, MAIS-3.1 is #12, and
so on down each milestone table.

## M1 - Host-ready invocation surface

**Goal.** A host can hand a native facade to a hook, read a typed result back,
list a module's hooks, see per-hook timing, and render a traceback as clickable
frames.

**In scope.** Library API and its tests. **Out of scope.** Engine configuration
interpretation, UI widgets, and anything owned by `guillaume`.

**Exit criteria.**

- [ ] A hook can receive a host-owned object without pybind11 appearing in
      `headers/mais/`.
- [ ] A caller can read a value a script returned.
- [ ] A module's functions can be listed without invoking them.
- [ ] A failing `on_update` does not stop the host loop, and its cost is
      attributable to that hook.
- [ ] A consumer can address an individual traceback frame.
- [ ] The GIL/threading model is written down and matches the implementation.
- [ ] CI builds and tests on the supported Python versions and all three
      desktop OSes.
- [ ] The dependency boundary is enforced by a check, not only by review.

| ID | Issue | Roadmap |
| --- | --- | --- |
| MAIS-1.1 | Pass host objects as script arguments | MAIS-1.1 |
| MAIS-1.2 | Return typed values from script calls | MAIS-1.2 |
| MAIS-1.3 | List a module's hooks without invoking them | MAIS-1.3 |
| MAIS-1.4 | Per-hook call policy and timing | MAIS-1.4 |
| MAIS-1.5 | Expose traceback frames as structured data | MAIS-1.5 |
| MAIS-1.6 | Decide and document the GIL and threading model | MAIS-1.7 |
| MAIS-1.7 | Verify the Python and OS support matrix in CI | MAIS-1.8 |
| MAIS-1.8 | Guard the dependency boundary automatically | new |

## M2 - Prove and document the consumer integration path

**Goal.** The story a `guillaume` implementer follows is written down and
executable, without adding engine or UI code to maïs.

**In scope.** A second maïs example and maïs documentation. **Out of scope.**
The adapter, the UI widgets, and engine configuration, all of which belong to
`guillaume` (see "Work tracked elsewhere" below).

**Exit criteria.**

- [ ] A frame-loop host example exercises configure, start, per-frame update,
      and shutdown, with a validated settings facade.
- [ ] The adapter pattern and the hook-schedule contract are documented, so a
      consumer knows what to implement and where hooks run.
- [ ] Script packaging (development versus shipped layout) is documented.

| ID | Issue | Roadmap |
| --- | --- | --- |
| MAIS-2.1 | Add a frame-loop host example with a validated settings facade | MAIS-2.1 |
| MAIS-2.2 | Document the host adapter pattern and the hook schedule | ADAPTER-2.1, HOST-2.3 |
| MAIS-2.3 | Document script packaging and resolution order | HOST-2.6 |

## M3 - Reload scripts without restarting the headset app

**Goal.** Edit a script, see the change in a running session, repeatedly, without
leaks and without dropping frames beyond a documented budget.

**Exit criteria.**

- [ ] Requirements, ownership, and failure modes for reload are written down
      before any implementation.
- [ ] A module can be reloaded and unloaded explicitly, with old objects
      released and no dangling native wrappers.
- [ ] Script changes can be detected and applied within a stated frame budget.
- [ ] State migration is explicit, opt-in, and schema-checked.
- [ ] A soak test loops many reloads under ASan without leaks.

| ID | Issue | Roadmap |
| --- | --- | --- |
| MAIS-3.1 | Design hot reload and state migration | MAIS-3.1 |
| MAIS-3.2 | Add explicit module reload and unload | MAIS-1.6, MAIS-3.4 |
| MAIS-3.3 | Detect script changes and apply them within a frame budget | MAIS-3.2 |
| MAIS-3.4 | Add an opt-in state migration contract | MAIS-3.3 |

## M4 - Survive long XR sessions

**Goal.** A session that runs for hours, with a script that may be wrong, on
constrained hardware, degrades instead of crashing.

**Exit criteria.**

- [ ] Scripts can be dispatched from a thread other than the initiator, if M1
      concluded that is needed.
- [ ] Memory behaviour across long sessions and reloads is measured, not
      assumed.
- [ ] A repeatedly failing hook has a defined policy and cannot take the
      session down.
- [ ] The trust model is decided, so sandboxing work is either justified or
      explicitly declined.
- [ ] Diagnostics can be recorded without a UI.

| ID | Issue | Roadmap |
| --- | --- | --- |
| MAIS-4.1 | Dispatch script calls from a worker thread | MAIS-4.1 |
| MAIS-4.2 | Measure memory across long sessions and reloads | MAIS-4.2 |
| MAIS-4.3 | Define the policy for repeatedly failing hooks | MAIS-4.3 |
| MAIS-4.4 | Decide the script trust model | MAIS-4.4 |
| MAIS-4.5 | Record diagnostics without a UI | MAIS-4.6 |

## M5 - Breadth

Conditional. No issue is created yet, because a single consumer must not shape
the seam. Gated on question Q7 in `ROADMAP.md` (a second scripting language has a
real sponsor) and Q8 (more than one runtime per process).

## Work tracked elsewhere

These roadmap items matter to the project but must **not** be implemented in
this repository. They belong to `guillaume` or to an optional `evan-scripting`
adapter, so they are tracked in their own project.

| Roadmap | Item | Owner |
| --- | --- | --- |
| ADAPTER-2.1 | Narrow engine configuration facade | `evan-scripting` adapter |
| ADAPTER-2.2 | Validate script-proposed settings before engine init | adapter + `guillaume` |
| HOST-2.3 | Hook schedule at exact loop points, respecting frame pacing | `guillaume` |
| HOST-2.4 | Script console fed by maïs diagnostics | `guillaume` UI |
| HOST-2.5 | `EngineSettings` panel | `guillaume` UI |
| HOST-2.7 | `SceneViewport` | `guillaume`, blocked on renderer/window ownership |
| ENGINE-* | Session, rendering, windowing, frame pacing | `evan` |
