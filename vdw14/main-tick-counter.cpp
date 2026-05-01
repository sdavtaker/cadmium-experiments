/**
 * VDW14 Tick-Counter Experiment — Cadmium PDEVS
 *
 * Reproduces the causality error from VDW14 (Vicino, Dalle, Wainer, SIMUTOOLS
 * 2014).  The coupled model is:
 *
 *   M1 (tick_gen,  period 1/10) --> Counter.add
 *   M2 (reset_gen, period 1)    --> Counter.reset
 *
 * M2 fires every 1 simulated second; M1 fires 10 times per second.
 * Expected output from Counter: always 10.
 *
 * With float time, IEEE 754 accumulation error on repeated addition of 0.1f
 * occasionally shifts a M1 event across a M2 boundary, producing 9 or 11.
 *
 * Reference: hal-01055555
 */

#include <algorithm>
#include <cadmium/basic_model/pdevs/accumulator.hpp>
#include <cadmium/engine/pdevs_coordinator.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/message_bag.hpp>
#include <cadmium/modeling/ports.hpp>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

using namespace cadmium;

// ── Port and value aliases ────────────────────────────────────────────────────

using acc_defs   = basic_models::pdevs::accumulator_defs<int>;
using reset_tick = acc_defs::reset_tick;

template <typename TIME>
using counter = basic_models::pdevs::accumulator<int, TIME>;

// ── tick_gen: fires every TIME{1}/TIME{10}, outputs int(1) ───────────────────

struct tick_gen_defs {
    struct out : public out_port<int> {};
};

template <typename TIME>
class tick_gen {
    using defs = tick_gen_defs;

public:
    using state_type  = int;
    state_type state  = 0;
    using input_ports = std::tuple<>;
    using output_ports = std::tuple<defs::out>;

    TIME period() const { return TIME{1} / TIME{10}; }

    constexpr tick_gen() noexcept = default;

    void internal_transition() {}

    void external_transition(TIME, make_message_bags<input_ports>::type) {
        throw std::logic_error("tick_gen has no input ports");
    }

    void confluence_transition(TIME, make_message_bags<input_ports>::type) {
        throw std::logic_error("tick_gen has no input ports");
    }

    make_message_bags<output_ports>::type output() const {
        make_message_bags<output_ports>::type bags;
        get_messages<defs::out>(bags).push_back(1);
        return bags;
    }

    TIME time_advance() const { return period(); }
};

// ── reset_gen: fires every TIME{1}, outputs reset_tick ───────────────────────

struct reset_gen_defs {
    struct out : public out_port<reset_tick> {};
};

template <typename TIME>
class reset_gen {
    using defs = reset_gen_defs;

public:
    using state_type   = int;
    state_type state   = 0;
    using input_ports  = std::tuple<>;
    using output_ports = std::tuple<defs::out>;

    constexpr reset_gen() noexcept = default;

    void internal_transition() {}

    void external_transition(TIME, make_message_bags<input_ports>::type) {
        throw std::logic_error("reset_gen has no input ports");
    }

    void confluence_transition(TIME, make_message_bags<input_ports>::type) {
        throw std::logic_error("reset_gen has no input ports");
    }

    make_message_bags<output_ports>::type output() const {
        make_message_bags<output_ports>::type bags;
        get_messages<defs::out>(bags).push_back(reset_tick{});
        return bags;
    }

    TIME time_advance() const { return TIME{1}; }
};

// ── Coupled top model ─────────────────────────────────────────────────────────

using empty_iports = std::tuple<>;
using empty_eic    = std::tuple<>;

using top_out_port = acc_defs::sum;
using top_oports   = std::tuple<top_out_port>;

using top_submodels = modeling::models_tuple<tick_gen, reset_gen, counter>;

using top_ic = std::tuple<
    modeling::IC<tick_gen,  tick_gen_defs::out,  counter, acc_defs::add>,
    modeling::IC<reset_gen, reset_gen_defs::out, counter, acc_defs::reset>
>;

using top_eoc = std::tuple<
    modeling::EOC<counter, acc_defs::sum, top_out_port>
>;

template <typename TIME>
using top_model = modeling::pdevs::coupled_model<
    TIME,
    empty_iports,
    top_oports,
    top_submodels,
    empty_eic,
    top_eoc,
    top_ic
>;

// ── Simulation driver ─────────────────────────────────────────────────────────

// Drives the coordinator manually to collect Counter outputs without using
// the runner (which discards output bags after each step).
template <typename TIME, template <typename> typename TopModel>
std::vector<int> run_experiment(TIME end_time) {
    engine::coordinator<TopModel, TIME> coord;
    coord.init(TIME{});
    TIME t = coord.next();
    std::vector<int> outputs;
    while (t < end_time) {
        coord.collect_outputs(t);
        const auto &vals = coord.template outbox_port<top_out_port>();
        outputs.insert(outputs.end(), vals.begin(), vals.end());
        coord.advance_simulation(t);
        t = coord.next();
    }
    return outputs;
}

// ── Statistics helper ─────────────────────────────────────────────────────────

static void print_stats(const char *label, const std::vector<int> &outputs) {
    if (outputs.empty()) {
        std::cout << label << ": no outputs\n";
        return;
    }

    long long errors = 0;
    std::map<int, long long> hist;
    for (int v : outputs) {
        ++hist[v];
        if (v != 10) ++errors;
    }

    int lo = hist.begin()->first;
    int hi = hist.rbegin()->first;

    std::cout << label << ":\n"
              << "  resets:  " << outputs.size() << "\n"
              << "  errors:  " << errors
              << " (" << (100.0 * errors / (double)outputs.size()) << " %)\n"
              << "  range:   [" << lo << ", " << hi << "]\n"
              << "  histogram:";
    for (const auto &[val, cnt] : hist)
        std::cout << "  " << val << "×" << cnt;
    std::cout << "\n\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    // 10 000 simulated seconds → 10 000 resets; enough to show float drift.
    const float  end_float  = 10000.0f;

    std::cout << "VDW14 Tick-Counter Experiment (Cadmium PDEVS)\n"
              << "M1 period=1/10  M2 period=1  expected counter output=10\n"
              << "end_time=" << end_float << "  resets=" << (long)end_float << "\n\n";

    auto float_outputs = run_experiment<float, top_model>(end_float);
    print_stats("float", float_outputs);

    return 0;
}
