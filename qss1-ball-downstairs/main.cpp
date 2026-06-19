// SPDX-License-Identifier: BSD-2-Clause
/**
 * K04 §4.2 Bouncing Ball Downstairs — QSS1 Experiment
 *
 * Replicates example 4.2 from Kofman (SIAM J. Sci. Comput., 2004).
 * Model topology and parameters are in ball_model.hpp.
 *
 * Output: NDJSON log to stdout (cadmium structured logging).
 *   Pass --quiet to suppress logging (e.g. for CI smoke tests).
 */

#include <cadmium/engine/devs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>

#include "ball_model.hpp"
#include <string_view>

int main(int argc, char **argv) {
    bool quiet = (argc > 1 && std::string_view(argv[1]) == "--quiet");
    if (!quiet)
        cadmium::log::init();

    cadmium::engine::devs::runner<TIME, bouncing_ball> r{0.0};
    r.run_until(10.0);

    if (!quiet)
        cadmium::log::flush();
    return 0;
}
