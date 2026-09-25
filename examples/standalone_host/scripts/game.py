"""Script driven by the standalone example host.

The host injects ``host.settings`` and ``host.metrics`` before this module is
imported, and calls the hooks it chooses to call.
"""

import host


def configure():
    host.settings.window_title = "mais standalone host"
    host.settings.width = 1280
    host.settings.height = 720


def on_start():
    print("game: started", host.settings.window_title)


def on_update(delta_seconds):
    host.metrics.frames += 1


def on_shutdown():
    print("game: shutting down after", host.metrics.frames, "frames")
