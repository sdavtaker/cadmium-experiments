// SPDX-License-Identifier: BSD-2-Clause
/**
 * bt-utp-s4-anti-snub-det: standalone driver for the anti-snub scenario
 * (models/client/s4_anti_snub_det.hpp), mirroring main_s4_det.cpp.
 *
 * Usage: bt-utp-s4-anti-snub-det [t_max_seconds]
 */
#include "models/client/s4_anti_snub_det.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
    const double t_max = argc > 1 ? std::stod(argv[1]) : 100.0;
    const auto result  = bt_utp::run_s4as_det(t_max);
    std::cout << result.ndjson_log;
    std::cerr << "finish=" << result.finish << "\n";
    return 0;
}
