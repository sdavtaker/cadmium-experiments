// SPDX-License-Identifier: BSD-2-Clause
/**
 * bt-utp-s3-det: standalone driver for the deterministic two-client
 * end-to-end scenario (models/client/s3_two_client_det.hpp), for
 * scripts/trace_audit_s3.py to run via subprocess and audit the NDJSON
 * trace on stdout — the "every BEP 3 exchange is visible in the log"
 * acceptance requirement.
 *
 * Usage: bt-utp-s3-det [t_max_seconds] [--full-trace]
 *
 * --full-trace additionally includes "sim_state" events (socket cwnd, etc.)
 * in the NDJSON trace — off by default since it multiplies trace volume
 * ~15x (see run_s3_det's doc comment); scripts/run_experiment.sh passes it
 * when cwnd data is needed.
 */
#include "models/client/s3_two_client_det.hpp"
#include <cstdlib>
#include <iostream>
#include <string_view>

int main(int argc, char **argv) {
    double t_max    = 200.0;
    bool full_trace = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--full-trace") {
            full_trace = true;
        } else {
            t_max = std::stod(argv[i]);
        }
    }
    const auto result = bt_utp::run_s3_det(t_max, full_trace);
    std::cout << result.ndjson_log;
    std::cerr << "finish=" << result.finish << "\n";
    return 0;
}
