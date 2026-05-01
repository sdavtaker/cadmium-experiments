/**
 * VDW14 Tick-Counter Experiment — Cadmium Classic DEVS
 *
 * Classic (sequential) DEVS variant of the VDW14 experiment.
 * Same 3-model topology as the PDEVS version but events are processed one at
 * a time; SELECT resolves ties when multiple models are imminent.
 *
 * SELECT priority: G_1/10 > G_1 > Counter
 *
 * Key contrast with PDEVS: SELECT only fires when models are *exactly*
 * simultaneous.  Float arithmetic makes near-simultaneous events
 * non-simultaneous, so SELECT is irrelevant for the error case — the
 * causality violation occurs before SELECT is consulted.
 *
 * Cadmium does not yet ship a general Classic DEVS coordinator; this
 * experiment uses a bespoke fixed-topology loop (see spec.tex).
 *
 * Reference: hal-01055555
 */

#include <algorithm>
#include <cadmium/basic_model/devs/accumulator.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace cadmium;

// ── Aliases ───────────────────────────────────────────────────────────────────

using acc_defs   = basic_models::devs::accumulator_defs<int>;
using reset_tick = acc_defs::reset_tick;

template <typename TIME>
using counter_t = basic_models::devs::accumulator<int, TIME>;

// ── tick_gen: Classic DEVS generator, period TIME{1}/TIME{10}, output int(1) ─

struct tick_gen_defs {
    struct out : public out_port<int> {};
};

template <typename TIME>
class tick_gen {
    using defs = tick_gen_defs;

public:
    using state_type   = int;
    state_type state   = 0;
    using input_ports  = std::tuple<>;
    using output_ports = std::tuple<defs::out>;

    TIME period() const { return TIME{1} / TIME{10}; }

    constexpr tick_gen() noexcept = default;

    void internal_transition() {}

    void external_transition(TIME, make_message_box<input_ports>::type) {
        throw std::logic_error("tick_gen has no input ports");
    }

    make_message_box<output_ports>::type output() const {
        make_message_box<output_ports>::type box;
        get_message<defs::out>(box).emplace(1);
        return box;
    }

    TIME time_advance() const { return period(); }
};

// ── reset_gen: Classic DEVS generator, period TIME{1}, output reset_tick ─────

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

    void external_transition(TIME, make_message_box<input_ports>::type) {
        throw std::logic_error("reset_gen has no input ports");
    }

    make_message_box<output_ports>::type output() const {
        make_message_box<output_ports>::type box;
        get_message<defs::out>(box).emplace(reset_tick{});
        return box;
    }

    TIME time_advance() const { return TIME{1}; }
};

// ── Bespoke Classic DEVS driving loop ─────────────────────────────────────────
//
// SELECT priority: M1 (tick_gen) > M2 (reset_gen) > counter
// Tracks absolute scheduled times and last-event times per model.

template <typename TIME>
std::vector<int> run_devs_experiment(TIME end_time) {
    tick_gen<TIME>  m1;
    reset_gen<TIME> m2;
    counter_t<TIME> counter;

    const TIME INF = std::numeric_limits<TIME>::infinity();

    // Absolute scheduled time for next internal event of each model.
    TIME sched_m1      = m1.time_advance();       // TIME{1}/TIME{10}
    TIME sched_m2      = m2.time_advance();       // TIME{1}
    TIME sched_counter = INF;

    // Time of last transition for the counter (used to compute elapsed).
    TIME last_counter = TIME{};

    std::vector<int> outputs;

    while (true) {
        TIME t_next = std::min({sched_m1, sched_m2, sched_counter});
        if (t_next >= end_time) break;

        // SELECT: process one model per iteration until none remain imminent.
        // Repeat at the same t_next until no model is scheduled there.

        // Which model fires? Apply SELECT priority.
        if (sched_m1 == t_next) {
            // --- M1 fires ---
            auto out = m1.output();
            m1.internal_transition();
            sched_m1 = t_next + m1.time_advance();

            // Route M1.out → Counter.add
            if (auto v = get_message<tick_gen_defs::out>(out)) {
                if (sched_counter != TIME{}) { // counter is not in ready state
                    typename make_message_box<typename counter_t<TIME>::input_ports>::type inbox{};
                    get_message<acc_defs::add>(inbox).emplace(*v);
                    counter.external_transition(t_next - last_counter, inbox);
                    last_counter   = t_next;
                    sched_counter  = t_next + counter.time_advance();
                }
            }

        } else if (sched_m2 == t_next) {
            // --- M2 fires ---
            auto out = m2.output();
            m2.internal_transition();
            sched_m2 = t_next + m2.time_advance();

            // Route M2.out → Counter.reset
            if (get_message<reset_gen_defs::out>(out).has_value()) {
                typename make_message_box<typename counter_t<TIME>::input_ports>::type inbox{};
                get_message<acc_defs::reset>(inbox).emplace(reset_tick{});
                counter.external_transition(t_next - last_counter, inbox);
                last_counter  = t_next;
                sched_counter = t_next + counter.time_advance();
            }

        } else {
            // --- Counter fires internally (ta==0 after reset) ---
            auto out = counter.output();
            counter.internal_transition();
            last_counter  = t_next;
            sched_counter = INF; // counter passivates after reset cycle

            // Route Counter.sum → top output (EOC)
            if (auto v = get_message<acc_defs::sum>(out)) {
                outputs.push_back(*v);
            }
        }
    }

    return outputs;
}

// ── Statistics ────────────────────────────────────────────────────────────────

static void print_stats(const char *label, const std::vector<int> &outputs) {
    if (outputs.empty()) { std::cout << label << ": no outputs\n"; return; }

    long long errors = 0;
    std::map<int, long long> hist;
    for (int v : outputs) { ++hist[v]; if (v != 10) ++errors; }

    std::cout << label << ":\n"
              << "  resets:  " << outputs.size() << "\n"
              << "  errors:  " << errors
              << " (" << (100.0 * errors / (double)outputs.size()) << " %)\n"
              << "  range:   [" << hist.begin()->first
              << ", " << hist.rbegin()->first << "]\n"
              << "  histogram:";
    for (const auto &[val, cnt] : hist)
        std::cout << "  " << val << "x" << cnt;
    std::cout << "\n\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    const float end_float = 10000.0f;

    std::cout << "VDW14 Tick-Counter Experiment (Cadmium Classic DEVS)\n"
              << "SELECT: G_1/10 > G_1 > Counter\n"
              << "M1 period=1/10  M2 period=1  expected counter output=10\n"
              << "end_time=" << end_float
              << "  resets=" << (long)end_float << "\n\n";

    auto float_outputs = run_devs_experiment<float>(end_float);
    print_stats("float", float_outputs);

    return 0;
}
