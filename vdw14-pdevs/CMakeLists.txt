cadmium_add_time_variants(vdw14-pdevs main.cpp)

# Tests
add_executable(vdw14-pdevs-tests test_models.cpp)
target_link_libraries(vdw14-pdevs-tests PRIVATE cadmium::cadmium Catch2::Catch2WithMain)
add_test(NAME vdw14-pdevs-tests COMMAND vdw14-pdevs-tests)
