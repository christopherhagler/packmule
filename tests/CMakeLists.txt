# Tests link against packmule_core (the OBJECT library) so they exercise the
# real implementations without linking a second copy of main().

# assert() is the test verification mechanism — keep it active in all build
# types by removing the -DNDEBUG flag that Release/MinSizeRel normally add.
add_compile_options(-UNDEBUG)

add_executable(test_package test_package.c)
target_link_libraries(test_package PRIVATE packmule_core)
add_test(NAME test_package COMMAND test_package)

add_executable(test_registry test_registry.c)
target_link_libraries(test_registry PRIVATE packmule_core)
add_test(NAME test_registry COMMAND test_registry)

add_executable(test_resolve test_resolve.c)
target_link_libraries(test_resolve PRIVATE packmule_core)
add_test(NAME test_resolve COMMAND test_resolve)

add_executable(test_rpm_repo test_rpm_repo.c)
target_link_libraries(test_rpm_repo PRIVATE packmule_core)
add_test(NAME test_rpm_repo COMMAND test_rpm_repo)

add_executable(test_semver test_semver.c)
target_link_libraries(test_semver PRIVATE packmule_core)
add_test(NAME test_semver COMMAND test_semver)

add_executable(test_pep440 test_pep440.c)
target_link_libraries(test_pep440 PRIVATE packmule_core)
add_test(NAME test_pep440 COMMAND test_pep440)

add_executable(test_pep508 test_pep508.c)
target_link_libraries(test_pep508 PRIVATE packmule_core)
add_test(NAME test_pep508 COMMAND test_pep508)

add_executable(test_wheeltag test_wheeltag.c)
target_link_libraries(test_wheeltag PRIVATE packmule_core)
add_test(NAME test_wheeltag COMMAND test_wheeltag)

add_executable(test_registry_pypi test_registry_pypi.c)
target_link_libraries(test_registry_pypi PRIVATE packmule_core)
add_test(NAME test_registry_pypi COMMAND test_registry_pypi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_registry_npm test_registry_npm.c)
target_link_libraries(test_registry_npm PRIVATE packmule_core)
add_test(NAME test_registry_npm COMMAND test_registry_npm
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_registry_rpm test_registry_rpm.c)
target_link_libraries(test_registry_rpm PRIVATE packmule_core)
add_test(NAME test_registry_rpm COMMAND test_registry_rpm
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_hash test_hash.c)
target_link_libraries(test_hash PRIVATE packmule_core)
add_test(NAME test_hash COMMAND test_hash
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_bundle test_bundle.c)
target_link_libraries(test_bundle PRIVATE packmule_core)
add_test(NAME test_bundle COMMAND test_bundle
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_auth test_auth.c)
target_link_libraries(test_auth PRIVATE packmule_core)
add_test(NAME test_auth COMMAND test_auth)

add_executable(test_simple_index test_simple_index.c)
target_link_libraries(test_simple_index PRIVATE packmule_core)
add_test(NAME test_simple_index COMMAND test_simple_index)

add_executable(test_pypi_metadata test_pypi_metadata.c)
target_link_libraries(test_pypi_metadata PRIVATE packmule_core)
add_test(NAME test_pypi_metadata COMMAND test_pypi_metadata)

add_executable(test_sbom test_sbom.c)
target_link_libraries(test_sbom PRIVATE packmule_core)
add_test(NAME test_sbom COMMAND test_sbom)

# ── End-to-end tests (network-dependent, opt-in) ─────────────────────────────
#
# Full pipeline against the real registries: resolve → download → bundle →
# extract → offline install (pypi: fresh venv with --no-index; npm: registry
# pointed at an unroutable address).  Off by default because they need
# network access; enable with -DPACKMULE_E2E_TESTS=ON and run via
# `ctest -L e2e`.  A missing prerequisite (python3/npm) skips, not fails.
option(PACKMULE_E2E_TESTS "Enable network-dependent end-to-end tests" OFF)
if(PACKMULE_E2E_TESTS)
    foreach(backend pypi npm rpm)
        add_test(NAME e2e_${backend}
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/e2e/e2e_${backend}.sh)
        set_tests_properties(e2e_${backend} PROPERTIES
            ENVIRONMENT "PACKMULE=$<TARGET_FILE:packmule>"
            SKIP_RETURN_CODE 77
            LABELS e2e
            TIMEOUT 600)
    endforeach()
endif()
