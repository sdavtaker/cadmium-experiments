// SPDX-License-Identifier: BSD-2-Clause
#include <cadmium/basic_model/devs/accumulator.hpp>
#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/engine/devs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/coupling.hpp>

#include "reset_gen.hpp"
#include "tick_gen.hpp"

using namespace cadmium;

template <typename TIME> using counter_t = basic_models::devs::accumulator<int, TIME>;

using empty_iports = std::tuple<>;
using empty_eic    = std::tuple<>;
using top_out_port = acc_defs::sum;
using top_oports   = std::tuple<top_out_port>;

// SELECT priority: tick_gen (index 0) > reset_gen (index 1) > counter_t (index 2)
// first_imminent picks the lowest-index imminent model, which encodes this ordering.
using top_submodels = modeling::models_tuple<tick_gen, reset_gen, counter_t>;

using top_ic = std::tuple<modeling::IC<tick_gen, tick_gen_defs::out, counter_t, acc_defs::add>,
                          modeling::IC<reset_gen, reset_gen_defs::out, counter_t, acc_defs::reset>>;

using top_eoc = std::tuple<modeling::EOC<counter_t, acc_defs::sum, top_out_port>>;

template <typename TIME>
using top_devs_model =
    modeling::devs::coupling<TIME, empty_iports, top_oports, top_submodels, empty_eic, top_eoc,
                             top_ic, engine::devs::first_imminent>;

int main() {
    cadmium::log::init();
    cadmium::engine::devs::runner<float, top_devs_model> r{0.0f};
    r.run_until(10000.0f);
    return 0;
}
