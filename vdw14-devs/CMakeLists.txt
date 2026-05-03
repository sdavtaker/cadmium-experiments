add_executable(vdw14-devs-tick-counter main.cpp)
target_link_libraries(vdw14-devs-tick-counter PRIVATE cadmium)
target_compile_options(vdw14-devs-tick-counter PRIVATE -Wall -Wextra -pedantic)

# Tests
add_executable(vdw14-devs-tests test_models.cpp)
target_link_libraries(vdw14-devs-tests PRIVATE cadmium Catch2::Catch2WithMain)
add_test(NAME vdw14-devs-tests COMMAND vdw14-devs-tests)
