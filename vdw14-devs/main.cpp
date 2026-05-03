#include <cadmium/basic_model/devs/accumulator.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "reset_gen.hpp"
#include "tick_gen.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <vector>

using namespace cadmium;

template <typename TIME> using counter_t = basic_models::devs::accumulator<int, TIME>;

// ── Bespoke Classic DEVS driving loop ─────────────────────────────────────────
//
// SELECT priority: tick_gen > reset_gen > counter

template <typename TIME> std::vector<int> run_devs_experiment(TIME end_time) {
    tick_gen<TIME> m1;
    reset_gen<TIME> m2;
    counter_t<TIME> counter;

    const TIME INF = std::numeric_limits<TIME>::infinity();

    TIME sched_m1      = m1.time_advance();
    TIME sched_m2      = m2.time_advance();
    TIME sched_counter = INF;
    TIME last_counter  = TIME{};

    std::vector<int> outputs;

    while (true) {
        TIME t_next = std::min({sched_m1, sched_m2, sched_counter});
        if (t_next >= end_time)
            break;

        if (sched_m1 == t_next) {
            auto out = m1.output();
            m1.internal_transition();
            sched_m1 = t_next + m1.time_advance();

            if (auto v = get_message<tick_gen_defs::out>(out)) {
                if (sched_counter != TIME{}) {
                    typename make_message_box<typename counter_t<TIME>::input_ports>::type inbox{};
                    get_message<acc_defs::add>(inbox).emplace(*v);
                    counter.external_transition(t_next - last_counter, inbox);
                    last_counter  = t_next;
                    sched_counter = t_next + counter.time_advance();
                }
            }

        } else if (sched_m2 == t_next) {
            auto out = m2.output();
            m2.internal_transition();
            sched_m2 = t_next + m2.time_advance();

            if (get_message<reset_gen_defs::out>(out).has_value()) {
                typename make_message_box<typename counter_t<TIME>::input_ports>::type inbox{};
                get_message<acc_defs::reset>(inbox).emplace(reset_tick{});
                counter.external_transition(t_next - last_counter, inbox);
                last_counter  = t_next;
                sched_counter = t_next + counter.time_advance();
            }

        } else {
            auto out = counter.output();
            counter.internal_transition();
            last_counter  = t_next;
            sched_counter = INF;

            if (auto v = get_message<acc_defs::sum>(out)) {
                outputs.push_back(*v);
            }
        }
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
    const float end_float = 10000.0f;

    std::cout << "VDW14 Tick-Counter Experiment (Cadmium Classic DEVS)\n"
              << "SELECT: G_1/10 > G_1 > Counter\n"
              << "M1 period=1/10  M2 period=1  expected counter output=10\n"
              << "end_time=" << end_float << "  resets=" << (long)end_float << "\n\n";

    auto float_outputs = run_devs_experiment<float>(end_float);
    print_stats("float", float_outputs);

    return 0;
}
