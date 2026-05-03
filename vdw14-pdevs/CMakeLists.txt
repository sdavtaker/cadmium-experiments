add_executable(vdw14-pdevs-tick-counter main.cpp)
target_link_libraries(vdw14-pdevs-tick-counter PRIVATE cadmium)
target_compile_options(vdw14-pdevs-tick-counter PRIVATE -Wall -Wextra -pedantic)

# Tests
add_executable(vdw14-pdevs-tests test_models.cpp)
target_link_libraries(vdw14-pdevs-tests PRIVATE cadmium Catch2::Catch2WithMain)
add_test(NAME vdw14-pdevs-tests COMMAND vdw14-pdevs-tests)
