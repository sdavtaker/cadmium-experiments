add_executable(qss1-ball-downstairs main.cpp)
target_include_directories(qss1-ball-downstairs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qss1-ball-downstairs PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(qss1-ball-downstairs PRIVATE cadmium::cadmium)

add_test(NAME qss1-ball-downstairs-runs COMMAND qss1-ball-downstairs --quiet)
set_tests_properties(qss1-ball-downstairs-runs PROPERTIES TIMEOUT 30)
