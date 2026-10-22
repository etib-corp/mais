"""Scripts exercised by the fake-host integration tests.

The host injects ``host.counter``, a native FakeCounter, before this module is
imported.
"""

import host

# A module-level attribute that is not callable: hasFunction() must report a
# TypeMismatch, and listFunctions() must omit it.
answer = 42


def bump():
    host.counter.increment()


def bump_many(times):
    for _ in range(times):
        host.counter.increment()


def reset():
    host.counter.reset()


def count():
    return host.counter.count


def apply(label, enabled):
    if enabled:
        host.counter.increment()
    return label


def explode():
    raise ValueError("boom from Python")


def divide(dividend, divisor):
    return dividend / divisor


def recurse(depth):
    if depth <= 0:
        raise ValueError("deep boom")
    # Alternate lines so CPython's repeated-frame folding does not hide the
    # depth in the formatted traceback.
    if depth % 2:
        return recurse(depth - 1)
    return recurse(depth - 1)


def flag():
    return True


def nothing():
    return None


def identity(value):
    return value


def label():
    return "héllo maïs"


def huge():
    return 2 ** 80


def mapping():
    return {}
