"""A module whose hook surface is inspected without executing it.

The introspection paths must read this module's namespace directly. The
functions below raise if they are invoked, and __getattr__/__dir__ raise if the
implementation reaches for attributes through Python rather than the dict.
"""

from math import sqrt

settings = {"title": "demo"}
answer = 42


def __getattr__(name):
    if name == "dynamic_hook":
        raise AssertionError("introspection must not read attributes")
    raise AttributeError(name)


def __dir__():
    raise AssertionError("introspection must not call dir()")


def _hidden():
    raise AssertionError("private helpers must not be invoked")


def on_start():
    raise AssertionError("introspection must not invoke hooks")


def on_update(delta):
    raise AssertionError("introspection must not invoke hooks")


class Tool:
    pass
