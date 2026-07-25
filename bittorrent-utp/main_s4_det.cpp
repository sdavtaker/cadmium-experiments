// SPDX-License-Identifier: BSD-2-Clause
/**
 * bt-utp-s4-det: standalone driver for the stage-4 deterministic
 * two-client end-to-end scenario (models/client/s4_two_client_det.hpp),
 * mirroring main_s3_det.cpp.
 *
 * Usage: bt-utp-s4-det [t_max_seconds]
 */
#include "models/client/s4_two_client_det.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
    const double t_max = argc > 1 ? std::stod(argv[1]) : 100.0;
    const auto result  = bt_utp::run_s4_det(t_max);
    std::cout << result.ndjson_log;
    std::cerr << "finish=" << result.finish << "\n";
    return 0;
}
