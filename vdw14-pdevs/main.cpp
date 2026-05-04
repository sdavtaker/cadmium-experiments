// SPDX-License-Identifier: BSD-2-Clause
#include <cadmium/basic_model/pdevs/accumulator.hpp>
#include <cadmium/engine/pdevs_coordinator.hpp>
#include <cadmium/modeling/coupling.hpp>

#include "reset_gen.hpp"
#include "tick_gen.hpp"
#include <iostream>
#include <map>
#include <vector>

using namespace cadmium;

// ── Aliases ───────────────────────────────────────────────────────────────────

template <typename TIME> using counter = basic_models::pdevs::accumulator<int, TIME>;

// ── Coupled top model ─────────────────────────────────────────────────────────

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

// ── Simulation driver ─────────────────────────────────────────────────────────

template <typename TIME, template <typename> typename TopModel>
std::vector<int> run_experiment(int max_resets) {
    engine::coordinator<TopModel, TIME> coord;
    coord.init(TIME{});
    TIME t = coord.next();
    std::vector<int> outputs;
    outputs.reserve(max_resets);
    while (static_cast<int>(outputs.size()) < max_resets) {
        coord.collect_outputs(t);
        const auto &vals = coord.template outbox_port<top_out_port>();
        outputs.insert(outputs.end(), vals.begin(), vals.end());
        coord.advance_simulation(t);
        t = coord.next();
    }
    return outputs;
}

// ── Statistics ────────────────────────────────────────────────────────────────

static void print_stats(const char *label, const std::vector<int> &outputs) {
    if (outputs.empty()) {
        std::cout << label << ": no outputs\n";
        return;
    }

    long long errors = 0;
    std::map<int, long long> hist;
    for (int v : outputs) {
        ++hist[v];
        if (v != 10)
            ++errors;
    }

    std::cout << label << ":\n"
              << "  resets:  " << outputs.size() << "\n"
              << "  errors:  " << errors << " (" << (100.0 * errors / (double)outputs.size())
              << " %)\n"
              << "  range:   [" << hist.begin()->first << ", " << hist.rbegin()->first << "]\n"
              << "  histogram:";
    for (const auto &[val, cnt] : hist)
        std::cout << "  " << val << "x" << cnt;
    std::cout << "\n\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    const int max_resets = 100000;

    std::cout << "VDW14 Tick-Counter Experiment (Cadmium PDEVS)\n"
              << "M1 period=1/10  M2 period=1  expected counter output=10\n"
              << "max_resets=" << max_resets << "\n\n";

    auto float_outputs = run_experiment<float, top_model>(max_resets);
    print_stats("float", float_outputs);

    return 0;
}
