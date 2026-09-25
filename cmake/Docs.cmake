# Doxygen documentation helpers.
#
# Usage:
#   include(${CMAKE_SOURCE_DIR}/cmake/Docs.cmake)
#   add_project_docs()
#
# Only used when BUILD_DOCS (or BUILD_DOCS_ONLY) is enabled, so Doxygen is not
# a hard requirement for building the library.

find_package(Doxygen QUIET)

function(add_project_docs)
    if(NOT DOXYGEN_FOUND)
        message(WARNING "Doxygen was not found; the documentation target is unavailable.")
        return()
    endif()

    set(_doxyfile_in "${CMAKE_SOURCE_DIR}/docs/Doxyfile.in")
    set(_doxyfile "${CMAKE_BINARY_DIR}/docs/Doxyfile")

    if(NOT EXISTS "${_doxyfile_in}")
        message(WARNING "Missing ${_doxyfile_in}; the documentation target is unavailable.")
        return()
    endif()

    configure_file("${_doxyfile_in}" "${_doxyfile}" @ONLY)

    add_custom_target(docs_all
        COMMAND "${DOXYGEN_EXECUTABLE}" "${_doxyfile}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/docs"
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )

    add_custom_target(docs DEPENDS docs_all)
endfunction()
