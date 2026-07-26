// SPDX-License-Identifier: BSD-2-Clause
/**
 * bt-utp-s4-det: standalone driver for the stage-4 deterministic
 * two-client end-to-end scenario (models/client/s4_two_client_det.hpp),
 * mirroring main_s3_det.cpp.
 *
 * Usage: bt-utp-s4-det [t_max_seconds] [--full-trace]
 *
 * --full-trace additionally includes "sim_state" events (socket cwnd, etc.)
 * in the NDJSON trace — see main_s3_det.cpp's own usage comment.
 */
#include "models/client/s4_two_client_det.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    double t_max    = 100.0;
    bool full_trace = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--full-trace") {
            full_trace = true;
        } else {
            t_max = std::stod(argv[i]);
        }
    }
    const auto result = bt_utp::run_s4_det(t_max, full_trace);
    std::cout << result.ndjson_log;
    std::cerr << "finish=" << result.finish << "\n";
    return 0;
}
