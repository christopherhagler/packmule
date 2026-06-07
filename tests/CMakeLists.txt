# Tests link against packmule_core (the OBJECT library) so they exercise the
# real implementations without linking a second copy of main().

add_executable(test_package test_package.c)
target_link_libraries(test_package PRIVATE packmule_core)
add_test(NAME test_package COMMAND test_package)

add_executable(test_registry test_registry.c)
target_link_libraries(test_registry PRIVATE packmule_core)
add_test(NAME test_registry COMMAND test_registry)

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
