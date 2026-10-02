"""Script driven by the standalone example host.

The host hands its settings and metrics objects to the hooks directly, so this
module never imports the host bindings; it edits the configuration facade it
receives and reads the per-frame state it is given.
"""


def configure(settings):
    settings.window_title = "mais standalone host"
    settings.width = 1280
    settings.height = 720


def on_start():
    print("game: started")


def on_update(metrics, delta_seconds):
    metrics.frames += 1


def on_shutdown(metrics):
    print("game: shutting down after", metrics.frames, "frames")
