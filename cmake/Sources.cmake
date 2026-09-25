# Explicit source list (no file(GLOB)) for reproducible builds.
#
# Only the generic runtime lives here. Engine bindings, consumer scripts and UI
# components belong to the host application or to an optional adapter.

set(MAIS_SOURCES
    ${SOURCES_DIR}/BindingRegistry.cpp
    ${SOURCES_DIR}/Error.cpp
    ${SOURCES_DIR}/ScriptRuntime.cpp
)
