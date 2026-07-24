# BitTorrent/uTP swarm propagation experiment (staged build-up).
# Stage 2: uTP message types, deterministic bottleneck channel, the
# deterministic LEDBAT socket atomic, and the STDEVS lossy-channel
# stochastic pass. Stage 3: BEP 3 message set, peer_wire FSM, stub
# policies, and the bittorrent_client coupled model.

add_executable(bt-utp-tests test/test_msg_channel.cpp test/test_utp_socket.cpp
                            test/test_pair_validation.cpp test/test_lossy_channel.cpp
                            test/test_two_client_sto.cpp test/test_bep3.cpp
                            test/test_peer_wire.cpp test/test_bittorrent_client.cpp
                            test/test_s3_two_client_det.cpp test/test_event_filter_sink.cpp
                            test/test_choking_policy.cpp)
target_include_directories(bt-utp-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bt-utp-tests PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(bt-utp-tests PRIVATE cadmium::cadmium cdcommons::cdcommons Catch2::Catch2WithMain)
add_test(NAME bt-utp-tests COMMAND bt-utp-tests)

add_executable(bt-utp-s2-sto main_s2_sto.cpp)
target_include_directories(bt-utp-s2-sto PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bt-utp-s2-sto PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(bt-utp-s2-sto PRIVATE cadmium::cadmium cdcommons::cdcommons)

add_executable(bt-utp-s3-det main_s3_det.cpp)
target_include_directories(bt-utp-s3-det PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bt-utp-s3-det PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(bt-utp-s3-det PRIVATE cadmium::cadmium cdcommons::cdcommons)
