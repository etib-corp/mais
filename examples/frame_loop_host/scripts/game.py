"""Script driven by the frame-loop example host.

The host owns the settings facade, the frame context, and the fixed-step loop.
This module only proposes settings and reacts to the steps it is handed; it
never imports the host bindings.
"""


def configure(settings):
    settings.window_title = "mais frame loop host"
    settings.width = 1600
    settings.height = 900


def on_start(context):
    print("game: started at frame", context.index)


def on_update(context):
    # `context` is the host's native FrameContext object, not a scalar. Reading
    # `index` also proves it: a number would have no such attribute.
    #
    # Fail twice in a row on purpose. The host applies
    # FailurePolicy::ReportOnce, so the first exception is reported with its
    # traceback, the second is suppressed, and every planned frame still runs.
    if context.index in (3, 4):
        raise RuntimeError("deliberate on_update failure")

    context.script_steps += 1


def on_shutdown(context):
    print("game: shutting down after", context.script_steps, "script steps")
