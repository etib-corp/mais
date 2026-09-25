"""Scripts exercised by the fake-host integration tests.

The host injects ``host.counter``, a native FakeCounter, before this module is
imported.
"""

import host


def bump():
    host.counter.increment()


def bump_many(times):
    for _ in range(times):
        host.counter.increment()


def reset():
    host.counter.reset()


def count():
    return host.counter.count()


def apply(label, enabled):
    if enabled:
        host.counter.increment()
    return label


def explode():
    raise ValueError("boom from Python")


def divide(dividend, divisor):
    return dividend / divisor
