add_executable(qss1-ball-downstairs main.cpp)
target_include_directories(qss1-ball-downstairs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qss1-ball-downstairs PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(qss1-ball-downstairs PRIVATE cadmium::cadmium)

add_test(NAME qss1-ball-downstairs-runs COMMAND qss1-ball-downstairs --quiet)
set_tests_properties(qss1-ball-downstairs-runs PROPERTIES TIMEOUT 30)

add_executable(qss1-ball-downstairs-tests ball_test.cpp)
target_include_directories(qss1-ball-downstairs-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(qss1-ball-downstairs-tests PRIVATE -Wall -Wextra -pedantic)
target_link_libraries(qss1-ball-downstairs-tests PRIVATE cadmium::cadmium Catch2::Catch2WithMain)

add_test(NAME qss1-ball-downstairs-trajectory COMMAND qss1-ball-downstairs-tests)
# 30s was tuned against CI (passes in ~1.79s there) and is too tight on a
# slower local machine: times out at ~30.1-30.2s on a Raspberry Pi 4 in one
# run, 69.78s in another with no other load on the box -- this project's
# CI runner is ~15-40x faster than that Pi and the Pi's own run-to-run
# variance alone spans more than 2x (also visible in ckw10-lbm-runs: ~116s
# locally vs a few seconds on CI). Confirmed reproducible, not a
# regression (bead memory-vault-6sqc). 300s comfortably covers the
# observed variance while still catching a genuine multi-x slowdown or
# hang.
set_tests_properties(qss1-ball-downstairs-trajectory PROPERTIES TIMEOUT 300)
