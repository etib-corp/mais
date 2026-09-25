"""Scripts used by the benchmark harness."""

import host


def noop():
    pass


def with_delta(delta_seconds):
    pass


def tick():
    host.counter.value += 1
