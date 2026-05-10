// SPDX-License-Identifier: BSD-2-Clause
#include <cadmium/basic_model/pdevs/accumulator.hpp>
#include <cadmium/engine/pdevs_coordinator.hpp>
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/coupling.hpp>

#include "reset_gen.hpp"
#include "tick_gen.hpp"
#include <iostream>
#include <map>

#if defined(CADMIUM_TIME_FLOAT)
using SimTime                        = float;
static constexpr const char *VARIANT = "float";
#elif defined(CADMIUM_TIME_DOUBLE)
using SimTime                        = double;
static constexpr const char *VARIANT = "double";
#elif defined(CADMIUM_TIME_RATIONAL)
#include "rational_time.hpp"
using SimTime                        = RationalTime;
static constexpr const char *VARIANT = "rational";
#else
#error "Define CADMIUM_TIME_FLOAT, CADMIUM_TIME_DOUBLE, or CADMIUM_TIME_RATIONAL"
#endif

using namespace cadmium;

template <typename TIME> using counter = basic_models::pdevs::accumulator<int, TIME>;

using empty_iports  = std::tuple<>;
using empty_eic     = std::tuple<>;
using top_out_port  = acc_defs::sum;
using top_oports    = std::tuple<top_out_port>;
using top_submodels = modeling::models_tuple<tick_gen, reset_gen, counter>;

using top_ic = std::tuple<modeling::IC<tick_gen, tick_gen_defs::out, counter, acc_defs::add>,
                          modeling::IC<reset_gen, reset_gen_defs::out, counter, acc_defs::reset>>;

using top_eoc = std::tuple<modeling::EOC<counter, acc_defs::sum, top_out_port>>;

template <typename TIME>
using top_model = modeling::pdevs::coupled_model<TIME, empty_iports, top_oports, top_submodels,
                                                 empty_eic, top_eoc, top_ic>;

int main() {
    cadmium::log::init();
    spdlog::get("cadmium")->set_level(spdlog::level::warn);

    cadmium::engine::coordinator<top_model, SimTime> coord;
    coord.init(SimTime{0});
    auto next = coord.next();

    long long total = 0, errors = 0;
    std::map<int, long long> hist;

    const SimTime end{10000};
    while (next < end) {
        coord.collect_outputs(next);
        for (int v : coord.outbox_port<top_out_port>()) {
            ++total;
            ++hist[v];
            if (v != 10)
                ++errors;
        }
        coord.advance_simulation(next);
        next = coord.next();
    }

    std::cout << "VDW14 Tick-Counter Experiment (Cadmium PDEVS)\n"
              << "TIME type       : " << VARIANT << "\n"
              << "tick_gen period : 0.1 s   reset_gen period: 1 s\n"
              << "expected output : 10 at every reset\n"
              << "run_until       : 10000 s\n\n";

    if (hist.empty()) {
        std::cout << "  no outputs\n";
        return 0;
    }
    std::cout << "  resets  : " << total << "\n"
              << "  errors  : " << errors << " (" << (100.0 * errors / (double)total) << " %)\n"
              << "  range   : [" << hist.begin()->first << ", " << hist.rbegin()->first << "]\n"
              << "  histogram:";
    for (const auto &[val, cnt] : hist)
        std::cout << "  " << val << "x" << cnt;
    std::cout << "\n";
    return 0;
}
