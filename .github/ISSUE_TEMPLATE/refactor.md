---
name: Refactor request
about: Suggest and document a potential refactor.
title: 'refactor: my game changer refactor'
labels: enhancement

---

**As a refactor is often rude and hard to implement, please document the changes you propose in the better way you can.**

# Context
**Describe what problems is encountered and why it has to be fixed**

## Priority level
**Add enough elements that shows how much this refactor is important to be done**

# Description
**Explain what this refactor should do**

# Affected modules
**List the modules that are affected by this refactor, so we can be careful while coding it**

*If all the modules are affected, just check here: (so high priority is justified)*

- [ ] maïs
    - [ ] Runtime lifecycle (initialize / shutdown)
    - [ ] Bindings (embedded modules)
    - [ ] Script loading (search paths, imports)
    - [ ] Invocation (call / callOptional)
    - [ ] Errors and tracebacks
    - [ ] Build, packaging and exported targets

- [ ] Documentation
    - [ ] Getting started
    - [ ] Architecture
    - [ ] Error handling

- [ ] Tests, examples and benchmarks

## PoC (optional)

## Schemas / Designs (optional)

# Possible conflicts
**Talk about actual behavior and if it can affects another side of the code (so we can be careful while coding it)**