cmake_minimum_required(VERSION 3.15)
project(packmule VERSION 0.1.0 LANGUAGES C)

# Provides CMAKE_INSTALL_BINDIR, CMAKE_INSTALL_MANDIR, etc.
# Defaults: bin, share/man, lib, include — relative to CMAKE_INSTALL_PREFIX.
include(GNUInstallDirs)

# ── Language standard ─────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)          # -std=c11, not -std=gnu11

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Warning flags (compiler-agnostic interface target) ────────────────────────
#
# Using an INTERFACE target rather than add_compile_options() means the flags
# only apply to our own targets (those that link packmule_warnings), never to
# system or third-party headers.
#
# -Werror is gated behind PACKMULE_WERROR (default ON for development and CI).
# Distro packaging MUST configure with -DPACKMULE_WERROR=OFF: build systems like
# Fedora's inject their own hardening warnings and bump compilers regularly, so
# -Werror would turn any new warning into a fail-to-build-from-source.
option(PACKMULE_WERROR "Treat compiler warnings as errors" ON)

add_library(packmule_warnings INTERFACE)
target_compile_options(packmule_warnings INTERFACE
    # GCC / Clang / Apple Clang
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall;-Wextra;-Wpedantic
        -Wcast-qual;-Wwrite-strings;-Wmissing-prototypes
        $<$<BOOL:${PACKMULE_WERROR}>:-Werror>>
    # MSVC  (untested — contributions welcome)
    $<$<C_COMPILER_ID:MSVC>:
        /W4
        /wd4996  # suppress "deprecated" warnings for POSIX names
        $<$<BOOL:${PACKMULE_WERROR}>:/WX>>
)

# ── System dependencies ───────────────────────────────────────────────────────
#
# On macOS, several of our dependencies (libarchive, openssl, curl) are
# "keg-only" in Homebrew: installed but not symlinked into the default prefix,
# so find_package() can't see them out of the box.  Prepend their keg prefixes
# to CMAKE_PREFIX_PATH so a bare `cmake -S . -B build` configures cleanly.
# Anything the user passes on the command line still takes precedence.
if(APPLE)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        foreach(_pkg libarchive openssl curl)
            execute_process(
                COMMAND "${BREW_EXECUTABLE}" --prefix ${_pkg}
                OUTPUT_VARIABLE _brew_prefix
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_brew_prefix AND IS_DIRECTORY "${_brew_prefix}")
                list(APPEND CMAKE_PREFIX_PATH "${_brew_prefix}")
            endif()
        endforeach()
    endif()
endif()

find_package(CURL       REQUIRED)   # → CURL::libcurl
find_package(OpenSSL    REQUIRED)   # → OpenSSL::Crypto
find_package(LibArchive REQUIRED)   # → LibArchive::LibArchive

# cJSON: require the system-installed package.
# Install with: dnf install cjson-devel  /  apt-get install libcjson-dev  /  brew install cjson
find_package(cJSON REQUIRED)

# Normalize the cJSON target across package providers:
#   Homebrew (macOS):         "cjson"         (no namespace)
#   libcjson-dev (Ubuntu/Debian) / cjson-devel (Fedora): "cJSON::cjson"
if(TARGET cJSON::cjson)
    set(CJSON_TARGET cJSON::cjson)
elseif(TARGET cjson)
    set(CJSON_TARGET cjson)
else()
    message(FATAL_ERROR "cJSON found but no known import target (tried cJSON::cjson, cjson)")
endif()

# ── Embedded install scripts ──────────────────────────────────────────────────
#
# The per-backend install.sh bodies live as real, lint-able shell files under
# scripts/.  At build time they are converted into a C header of byte arrays so
# the binary stays self-contained (no runtime resource lookup).  See
# cmake/embed_scripts.cmake.
set(BUNDLE_SCRIPTS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/bundle_scripts.h")
add_custom_command(
    OUTPUT  "${BUNDLE_SCRIPTS_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            -DSCRIPTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/scripts
            -DOUTPUT=${BUNDLE_SCRIPTS_HEADER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_scripts.cmake
    DEPENDS scripts/install_pypi.sh
            scripts/install_npm.sh
            scripts/install_rpm.sh
            cmake/embed_scripts.cmake
    COMMENT "Embedding install scripts -> bundle_scripts.h"
    VERBATIM
)
add_custom_target(bundle_scripts DEPENDS "${BUNDLE_SCRIPTS_HEADER}")

# ── Core object library ───────────────────────────────────────────────────────
#
# Compiled once and reused by both the executable and the test binaries so
# main() is never linked into the test binaries.
add_library(packmule_core OBJECT
    src/utils.c
    src/package.c
    src/network.c
    src/hash.c
    src/registry.c
    src/registry_pypi.c
    src/registry_npm.c
    src/registry_rpm.c
    src/bundle.c
)

target_include_directories(packmule_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# bundle.c includes the generated bundle_scripts.h; make sure it is produced
# before packmule_core compiles and that the generated dir is on the include path.
add_dependencies(packmule_core bundle_scripts)
target_include_directories(packmule_core PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# _XOPEN_SOURCE=700 (SUSv4 / POSIX.1-2008 + XSI) unlocks all the POSIX APIs
# we use under strict -std=c11 on glibc: clock_gettime, CLOCK_MONOTONIC,
# realpath, PATH_MAX, fileno, mkdtemp, isatty.  It implies _POSIX_C_SOURCE=200809L
# on glibc, and also sets __USE_XOPEN_EXTENDED which is required for realpath
# (not covered by _POSIX_C_SOURCE alone).
target_compile_definitions(packmule_core PUBLIC _XOPEN_SOURCE=700)
# On macOS, _XOPEN_SOURCE hides Darwin-specific extensions unless
# _DARWIN_C_SOURCE is also defined to re-enable them.
target_compile_definitions(packmule_core PUBLIC
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE>)

target_link_libraries(packmule_core PUBLIC
    packmule_warnings       # strict warning flags for all our sources
    CURL::libcurl
    OpenSSL::Crypto
    LibArchive::LibArchive
    ${CJSON_TARGET}
)

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(packmule src/main.c)
target_link_libraries(packmule PRIVATE packmule_core)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

# ── Man page (Unix only) ──────────────────────────────────────────────────────
#
# PACKMULE_COMPRESS_MAN pre-compresses the page with gzip, which is convenient
# for `make install`/cpack.  Distro packaging should set -DPACKMULE_COMPRESS_MAN=OFF
# and let the packaging tooling compress (e.g. RPM's brp-compress), avoiding a
# double-compression conflict.
option(PACKMULE_COMPRESS_MAN "Pre-compress the installed man page with gzip" ON)

if(UNIX)
    find_program(GZIP_EXECUTABLE gzip)
    if(PACKMULE_COMPRESS_MAN AND GZIP_EXECUTABLE)
        set(MAN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/man/packmule.1")
        set(MAN_GZ     "${CMAKE_CURRENT_BINARY_DIR}/packmule.1.gz")
        add_custom_command(
            OUTPUT  "${MAN_GZ}"
            COMMAND "${GZIP_EXECUTABLE}" -9 -n -c "${MAN_SOURCE}" > "${MAN_GZ}"
            DEPENDS "${MAN_SOURCE}"
            COMMENT "Compressing man page"
            VERBATIM
        )
        add_custom_target(packmule_man ALL DEPENDS "${MAN_GZ}")
        install(FILES "${MAN_GZ}"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
    else()
        if(PACKMULE_COMPRESS_MAN AND NOT GZIP_EXECUTABLE)
            message(WARNING "gzip not found — installing uncompressed man page")
        endif()
        install(FILES man/packmule.1
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man1")
    endif()
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS packmule RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# ── Packaging (CPack) ─────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME                "packmule")
set(CPACK_PACKAGE_VERSION             "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Air-gapped multi-registry package bundler")
# Use a single string; multi-argument set() creates a semicolon-separated list.
set(CPACK_PACKAGE_DESCRIPTION
    "packmule reads a package manifest, resolves each entry against the \
appropriate upstream registry (PyPI, npm, RPM), downloads the full transitive \
set of packages, and verifies each file against its published SHA-256 digest. \
The result is a self-contained directory for offline installation.")
set(CPACK_PACKAGE_CONTACT      "Christopher Hagler <haglerchristopher@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/christopherhagler/packmule")
# set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

set(CPACK_GENERATOR "DEB;RPM;TGZ")

# ── Debian ────────────────────────────────────────────────────────────────────
set(CPACK_DEBIAN_PACKAGE_SECTION  "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME        DEB-DEFAULT)
# libcurl and libarchive were renamed in Ubuntu 24.04 (t64 ABI transition);
# list both with OR so the .deb installs cleanly on 22.04 and 24.04.
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libcurl4 (>= 7.68) | libcurl4t64 (>= 7.68), \
libssl3 (>= 3.0) | libssl1.1, \
libarchive13 | libarchive13t64")

# ── RPM (Fedora / RHEL / openSUSE) ───────────────────────────────────────────
set(CPACK_RPM_FILE_NAME        RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_GROUP    "Development/Tools")
set(CPACK_RPM_PACKAGE_LICENSE  "MIT")
set(CPACK_RPM_PACKAGE_REQUIRES "libcurl >= 7.68, openssl-libs >= 3.0, libarchive >= 3.4")
set(CPACK_RPM_PACKAGE_URL      "${CPACK_PACKAGE_HOMEPAGE_URL}")

# cJSON is a system dependency (find_package(cJSON REQUIRED) above); nothing is
# bundled.  If you tighten the dependency lists, add "libcjson1" (Debian) /
# "cjson" (RPM) to the respective DEPENDS lines above.
#
# NOTE: these CPACK_* dependency strings drive only the upstream `cpack` DEB/RPM
# and target recent Debian/Ubuntu/Fedora.  The Fedora/EPEL spec (packmule.spec)
# is the canonical packaging and relies on RPM's automatic dependency
# generation, so it works across RHEL 8/9/10 without these version pins.

include(CPack)
