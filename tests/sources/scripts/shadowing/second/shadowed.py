"""The other module named ``shadowed``.

See ``../first/shadowed.py``: the runtime must import whichever directory it was
given first, independent of the names of the directories.
"""

ORIGIN = "second"


def origin():
    return ORIGIN
