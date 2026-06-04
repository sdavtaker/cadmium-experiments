cadmium_add_time_variants(vdw14-devs main.cpp)

# Tests
add_executable(vdw14-devs-tests test_models.cpp)
target_link_libraries(vdw14-devs-tests PRIVATE cadmium::cadmium Catch2::Catch2WithMain)
add_test(NAME vdw14-devs-tests COMMAND vdw14-devs-tests)
