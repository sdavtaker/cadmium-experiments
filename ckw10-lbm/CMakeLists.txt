add_executable(ckw10-lbm main.cpp)
target_include_directories(ckw10-lbm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(ckw10-lbm PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(ckw10-lbm PRIVATE cadmium::cadmium)

add_test(NAME ckw10-lbm-runs COMMAND ckw10-lbm)
