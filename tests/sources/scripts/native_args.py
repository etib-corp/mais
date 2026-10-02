"""Scripts exercised by the native-argument tests.

The host hands a facade to the hook directly instead of exposing it as a module
attribute, so the hook is shaped like ``configure(settings)`` or
``on_update(context, delta_seconds)``.
"""

import facade


def configure(settings):
    settings.title = "configured by script"
    settings.width = 1280
    settings.height = 720


def observe_settings(settings):
    facade.observer.record_settings(settings)


def observe_context(context, delta_seconds):
    context.frame += 1
    context.elapsed += delta_seconds
    facade.observer.record_context(context)
