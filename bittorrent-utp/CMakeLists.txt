# BitTorrent/uTP swarm propagation experiment (staged build-up).
# Stage 2: uTP message types, deterministic bottleneck channel, the
# deterministic LEDBAT socket atomic, and the STDEVS lossy-channel
# stochastic pass. Stage 3: BEP 3 message set.

add_executable(bt-utp-tests test/test_msg_channel.cpp test/test_utp_socket.cpp
                            test/test_pair_validation.cpp test/test_lossy_channel.cpp
                            test/test_two_client_sto.cpp test/test_bep3.cpp)
target_include_directories(bt-utp-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bt-utp-tests PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(bt-utp-tests PRIVATE cadmium::cadmium cdcommons::cdcommons Catch2::Catch2WithMain)
add_test(NAME bt-utp-tests COMMAND bt-utp-tests)

add_executable(bt-utp-s2-sto main_s2_sto.cpp)
target_include_directories(bt-utp-s2-sto PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bt-utp-s2-sto PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(bt-utp-s2-sto PRIVATE cadmium::cadmium cdcommons::cdcommons)
