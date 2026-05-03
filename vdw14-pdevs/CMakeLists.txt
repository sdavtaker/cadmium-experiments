add_executable(vdw14-pdevs-tick-counter main-tick-counter.cpp)
target_link_libraries(vdw14-pdevs-tick-counter PRIVATE cadmium)
target_compile_options(vdw14-pdevs-tick-counter PRIVATE -Wall -Wextra -pedantic)
