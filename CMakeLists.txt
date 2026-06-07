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
# Using an INTERFACE target rather than add_compile_options() has two benefits:
#   1. The flags never leak into FetchContent'd third-party sources (cjson, etc.)
#      because those projects don't link to packmule_warnings.
#   2. Adding a new internal target automatically inherits the flags via the
#      PUBLIC dependency on packmule_core, without another add_compile_options()
#      call.
add_library(packmule_warnings INTERFACE)
target_compile_options(packmule_warnings INTERFACE
    # GCC / Clang / Apple Clang
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall;-Wextra;-Wpedantic;-Werror
        -Wcast-qual;-Wwrite-strings;-Wmissing-prototypes>
    # MSVC  (untested — contributions welcome)
    $<$<C_COMPILER_ID:MSVC>:
        /W4;/WX
        /wd4996>  # suppress "deprecated" warnings for POSIX names
)

# ── System dependencies ───────────────────────────────────────────────────────
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
if(UNIX)
    find_program(GZIP_EXECUTABLE gzip)
    if(GZIP_EXECUTABLE)
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
        message(WARNING "gzip not found — installing uncompressed man page")
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

# cJSON is statically linked (FetchContent) or dynamically linked (system).
# If the system cJSON is used, add "libcjson1" (Debian) / "cjson" (RPM) to the
# respective DEPENDS lines above.

include(CPack)
