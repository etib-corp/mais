"""One of two modules with the same name.

The search-path tests add this directory and ``../second`` to the runtime; each
directory holds a ``shadowed`` module, so the import resolves to whichever path
the runtime searches first.
"""

ORIGIN = "first"


def origin():
    return ORIGIN
