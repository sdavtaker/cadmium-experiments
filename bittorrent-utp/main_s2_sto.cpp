// SPDX-License-Identifier: BSD-2-Clause
/**
 * bt-utp-s2-sto: standalone driver for one instance of the stochastic
 * two-client scenario (models/utp/two_client_sto_scenario.hpp), for the
 * throughput-vs-loss statistical check that scripts/throughput_vs_loss.py
 * runs across many (seed, p) combinations — a trend across ≥10 seeds isn't
 * something a single ctest assertion can express.
 *
 * Usage: bt-utp-s2-sto <seed> <drop_prob>
 * Prints one line: seed=<> p=<> finish=<> delivered=<0|1> dropped_ab=<>
 * retransmits=<> throughput_bps=<>
 */
#include "models/utp/two_client_sto_scenario.hpp"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <seed> <drop_prob>\n";
        return 2;
    }
    const auto seed                     = static_cast<std::uint32_t>(std::stoul(argv[1]));
    const double drop_prob              = std::stod(argv[2]);
    constexpr std::uint64_t total_bytes = 2'000'000;

    const bt_utp::sto_scenario_result r = bt_utp::run_two_client_sto(seed, drop_prob, total_bytes);
    const double throughput_bps =
        r.finish > 0.0 ? static_cast<double>(total_bytes) / r.finish : 0.0;

    std::cout << "seed=" << seed << " p=" << drop_prob << " finish=" << r.finish
              << " delivered=" << (r.all_delivered ? 1 : 0) << " dropped_ab=" << r.dropped_ab
              << " retransmits=" << r.retransmits_a << " throughput_bps=" << throughput_bps << "\n";
    return r.all_delivered ? 0 : 1;
}
