# BitTorrent/uTP swarm propagation experiment (staged build-up).
# Stage 2: uTP message types, deterministic bottleneck channel, and the
# deterministic LEDBAT socket atomic.

add_executable(bt-utp-tests test/test_msg_channel.cpp test/test_utp_socket.cpp test/test_pair_validation.cpp)
target_include_directories(bt-utp-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bt-utp-tests PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(bt-utp-tests PRIVATE cadmium::cadmium Catch2::Catch2WithMain)
add_test(NAME bt-utp-tests COMMAND bt-utp-tests)
