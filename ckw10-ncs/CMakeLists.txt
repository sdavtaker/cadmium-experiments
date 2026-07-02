add_executable(ckw10-ncs main.cpp)
target_include_directories(ckw10-ncs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(ckw10-ncs PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(ckw10-ncs PRIVATE cadmium::cadmium)

add_test(NAME ckw10-ncs-runs COMMAND ckw10-ncs --quick)
