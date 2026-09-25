cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0077)
    cmake_policy(SET CMP0077 NEW)
endif()

project(mais
    VERSION 1.0.0
    DESCRIPTION "Reusable C++ scripting runtime embedding Python through pybind11"
    HOMEPAGE_URL "https://etib-corp.github.io/mais"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable CTest at the top level so tests are registered in the root
# CTestTestfile.cmake (enable_testing() in a subdirectory is not enough).
include(CTest)

# 01. Policy & standard setup

# 02. Options & configuration
option(BUILD_EXAMPLES "Build the standalone example hosts" OFF)
option(BUILD_BENCHMARKS "Build the benchmark harness" OFF)
option(BUILD_DOCS "Build Doxygen documentation" OFF)
option(BUILD_DOCS_ONLY "Configure only the documentation target" OFF)
option(BUILD_ALL_DOCS "Build documentation for all libraries" OFF)
option(DEV_MODE "Enable development mode (extra warnings, warnings as errors)" OFF)
option(MAIS_FETCH_PYBIND11 "Fetch pybind11 when it is not installed" ON)
option(MAIS_FETCH_GOOGLETEST "Fetch GoogleTest when it is not installed" ON)

if(BUILD_DOCS_ONLY)
    set(BUILD_DOCS ON)
    set(BUILD_TESTING OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_BENCHMARKS OFF)
endif()

# 03. Global properties

# Paths
set(HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/headers")
set(SOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sources")

include(GNUInstallDirs)

if(NOT BUILD_DOCS_ONLY)
    # 04. Dependencies
    #
    # pybind11 is the binding technology behind every host-facing feature.
    # The public headers deliberately do not include it, so consumers need it
    # to link, not to compile against maïs.
    set(PYBIND11_FINDPYTHON ON CACHE BOOL "" FORCE)
    set(PYBIND11_INSTALL OFF CACHE BOOL "" FORCE)
    set(PYBIND11_TEST OFF CACHE BOOL "" FORCE)
    set(MAIS_PYBIND11_TAG "v2.13.6")

    # Embedding links the Python runtime itself rather than the
    # extension-module shim, so a development installation is required.
    # pybind11 owns Python discovery for the whole build: a second
    # find_package(Python...) risks resolving the interpreter and the library
    # to different installations. Select an interpreter with
    # -DPython_EXECUTABLE=<path>.
    find_package(pybind11 CONFIG QUIET)
    if(NOT pybind11_FOUND)
        if(NOT MAIS_FETCH_PYBIND11)
            message(FATAL_ERROR
                "pybind11 was not found and MAIS_FETCH_PYBIND11 is OFF. "
                "Install pybind11 or reconfigure with -DMAIS_FETCH_PYBIND11=ON."
            )
        endif()

        include(FetchContent)
        FetchContent_Declare(pybind11
            GIT_REPOSITORY https://github.com/pybind/pybind11.git
            GIT_TAG ${MAIS_PYBIND11_TAG}
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
        )
        FetchContent_MakeAvailable(pybind11)
        set(pybind11_VERSION "${MAIS_PYBIND11_TAG}")
    endif()

    # pybind11 is not ABI-compatible across major versions, so the installed
    # package records the major it was built against and rejects a mismatch.
    string(REGEX REPLACE "^v" "" MAIS_PYBIND11_VERSION "${pybind11_VERSION}")
    string(REGEX MATCH "^[0-9]+" MAIS_PYBIND11_VERSION_MAJOR
        "${MAIS_PYBIND11_VERSION}"
    )

    # 05. Library definition
    include(cmake/Sources.cmake)

    add_library(${PROJECT_NAME} ${MAIS_SOURCES})
    add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

    # 06. Include directories
    target_include_directories(${PROJECT_NAME}
        PUBLIC
            $<BUILD_INTERFACE:${HEADERS_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    # 07. Compile options
    if(DEV_MODE)
        if(MSVC)
            target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
        else()
            target_compile_options(${PROJECT_NAME} PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Werror
            )
        endif()
    endif()

    # 08. Link dependencies
    #
    # PRIVATE keeps pybind11 out of the public compile interface while CMake
    # still propagates the link requirements to consumers of the static
    # library. pybind11::embed also carries the embeddable Python library and
    # its headers, which the runtime uses directly.
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            pybind11::embed
    )

    # 09. Packaging & consumability

    install(TARGETS ${PROJECT_NAME}
        EXPORT ${PROJECT_NAME}Targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY ${HEADERS_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT ${PROJECT_NAME}Targets
        FILE ${PROJECT_NAME}Targets.cmake
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        cmake/${PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )

    # CPack packaging
    set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
    set(CPACK_PACKAGE_VENDOR "ETIB Corporation")
    set(CPACK_PACKAGE_CONTACT "clement-antoine.xavier@epitech.eu")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    include(CPack)
endif()

# 10. Documentation

if(BUILD_DOCS)
    message(STATUS "Building documentation...")
    add_subdirectory(docs)
else()
    message(STATUS "Skipping documentation...")
endif()

# 11. Tests

if(BUILD_TESTING)
    message(STATUS "Building tests...")
    add_subdirectory(tests)
else()
    message(STATUS "Skipping tests...")
endif()

# 12. Benchmarks

if(BUILD_BENCHMARKS)
    message(STATUS "Building benchmarks...")
    add_subdirectory(benchmarks)
endif()

# 13. Examples

if(BUILD_EXAMPLES)
    message(STATUS "Building examples...")
    add_subdirectory(examples)
endif()
