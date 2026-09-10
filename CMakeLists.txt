cmake_minimum_required(VERSION 3.14)
project(PackageManagerLib LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LGPM_PORTABLE_BUILD "Build for portable variant selection (selects portable LGX variants instead of dev)" OFF)

# Find nlohmann_json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(json)
endif()

# Find lgx library
if(NOT DEFINED LGX_ROOT)
    message(FATAL_ERROR "LGX_ROOT must be set to the logos-package library path")
endif()
message(STATUS "Using lgx library at: ${LGX_ROOT}")

# Library sources
set(LIB_SOURCES
    src/package_manager_lib.cpp
    src/package_manager_lib.h
    src/package_manager_json.cpp
    src/package_manager_json.h
    src/lgpm.cpp
    src/lgpm.h
)

# Create the shared library
option(LGPM_STATIC_LIB "Build package_manager_lib as a static archive" OFF)
set(LGPM_LIB_TYPE SHARED)
if(LGPM_STATIC_LIB)
    set(LGPM_LIB_TYPE STATIC)
endif()
add_library(package_manager_lib ${LGPM_LIB_TYPE} ${LIB_SOURCES})

# The C API in src/lgpm.h is explicitly __declspec(dllexport)-ed, and GNU ld
# disables PE auto-export for the ENTIRE image as soon as any symbol is
# explicitly exported. The C++ classes the CLI links against carry no
# annotation, so they silently vanished from the export table and lgpm/lgpd
# failed to link with 42 undefined references to perfectly ordinary methods.
# Ask CMake to generate a .def covering everything else as well.
if(WIN32)
    # Ask ld directly rather than via WINDOWS_EXPORT_ALL_SYMBOLS. That property
    # generates a .def file, and under this cross toolchain CMake never emits
    # one -- no .def appears in the build and no objdump runs -- so it silently
    # exports nothing. (Supplying CMAKE_OBJDUMP, the usual cause, made no
    # difference; verified.) --export-all-symbols is ld's own switch and needs
    # none of that machinery.
    target_link_options(package_manager_lib PRIVATE -Wl,--export-all-symbols)
endif()


# Portable build flag
if(LGPM_PORTABLE_BUILD)
    target_compile_definitions(package_manager_lib PRIVATE LGPM_PORTABLE_BUILD=1)
endif()

# Export macro for shared library
target_compile_definitions(package_manager_lib PRIVATE LGPM_BUILD_SHARED=1)

set_target_properties(package_manager_lib PROPERTIES
    OUTPUT_NAME "package_manager_lib"
)

# Include directories
target_include_directories(package_manager_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(package_manager_lib PRIVATE
    ${LGX_ROOT}/include
)

# Link nlohmann_json and lgx
find_library(LGX_LIB lgx PATHS ${LGX_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
message(STATUS "Found lgx library: ${LGX_LIB}")

target_link_libraries(package_manager_lib PRIVATE
    nlohmann_json::nlohmann_json
    ${LGX_LIB}
)

# RPATH settings
set_target_properties(package_manager_lib PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH FALSE
)

if(APPLE AND NOT LGPM_STATIC_LIB)
    set_target_properties(package_manager_lib PROPERTIES
        INSTALL_RPATH "@loader_path;${LGX_ROOT}/lib"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE
    )
    add_custom_command(TARGET package_manager_lib POST_BUILD
        COMMAND install_name_tool -id "@rpath/libpackage_manager_lib.dylib" $<TARGET_FILE:package_manager_lib>
        COMMAND install_name_tool -change "${LGX_LIB}" "@rpath/liblgx.dylib" $<TARGET_FILE:package_manager_lib>
        COMMENT "Updating library paths for macOS"
    )
else()
    set_target_properties(package_manager_lib PROPERTIES
        INSTALL_RPATH "$ORIGIN:${LGX_ROOT}/lib"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )
endif()

# Set output directories
set_target_properties(package_manager_lib PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# CLI executable
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
add_subdirectory(cmd)
endif()

# Testing (optional)
option(LGPM_BUILD_TESTS "Build tests" OFF)
if(LGPM_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

# Install rules
install(TARGETS package_manager_lib
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES src/package_manager_lib.h src/package_manager_json.h src/lgpm.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
