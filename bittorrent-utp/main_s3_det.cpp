// SPDX-License-Identifier: BSD-2-Clause
/**
 * bt-utp-s3-det: standalone driver for the deterministic two-client
 * end-to-end scenario (models/client/s3_two_client_det.hpp), for
 * scripts/trace_audit_s3.py to run via subprocess and audit the NDJSON
 * trace on stdout — the "every BEP 3 exchange is visible in the log"
 * acceptance requirement.
 *
 * Usage: bt-utp-s3-det [t_max_seconds]
 */
#include "models/client/s3_two_client_det.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
    const double t_max = argc > 1 ? std::stod(argv[1]) : 200.0;
    const auto result  = bt_utp::run_s3_det(t_max);
    std::cout << result.ndjson_log;
    std::cerr << "finish=" << result.finish << "\n";
    return 0;
}
