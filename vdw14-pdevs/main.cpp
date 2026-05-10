// SPDX-License-Identifier: BSD-2-Clause
#include <cadmium/basic_model/pdevs/accumulator.hpp>
#include <cadmium/engine/pdevs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/coupling.hpp>

#include "reset_gen.hpp"
#include "tick_gen.hpp"

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
    cadmium::engine::runner<float, top_model> r{0.0f};
    r.run_until(10000.0f);
    return 0;
}
