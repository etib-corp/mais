"""Script whose settings proposal the host must reject.

The width is outside the host's accepted range, so the host reports the
proposal, keeps its previous settings, and starts the engine with those. The
invalid value never reaches the engine.
"""


def configure(settings):
    settings.window_title = "invalid frame loop host"
    settings.width = 0  # Outside 64..7680: the host must reject the proposal.
    settings.height = 720


def on_start(context):
    print("game: started at frame", context.index)


def on_update(context):
    context.script_steps += 1


def on_shutdown(context):
    print("game: shutting down after", context.script_steps, "script steps")
