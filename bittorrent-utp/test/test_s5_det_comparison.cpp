// SPDX-License-Identifier: BSD-2-Clause
/**
 * Stage-5 det comparison (bead zqns): stage-3 (stub always-unchoke +
 * sequential selector) and stage-4 (real choking_policy + piece_selector)
 * transfer the same 40 pieces over identically-parameterized channels
 * (see s3_two_client_det.hpp / s4_two_client_det.hpp: same total_pieces,
 * sub_pieces_per_piece, sub_piece_bytes, rate_ab, rate_ba, prop_delay).
 * test_s3_two_client_det.cpp and test_s4_two_client_det.cpp each already
 * pin their own stage's completion time individually; this file pins the
 * *comparison* between them, which neither of those checks on its own.
 *
 * This runs both full simulations a second time (test_s3/s4_two_client_det.cpp
 * already run each once for their own smoke+golden tests) -- an accepted
 * cost in this project already (see test_s4_two_client_det.cpp's own doc
 * comment on why a shared run isn't split across SECTIONs either).
 */
#include "../models/client/s3_two_client_det.hpp"
#include "../models/client/s4_two_client_det.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace {

    std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
        std::size_t count = 0;
        std::size_t pos   = 0;
        while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    // Completion time is the sim_time of the receiver's Nth distinct HAVE
    // announcement (see test_s4_two_client_det.cpp's track_have_progress for
    // the same logic) -- used for *both* s3 and s4 here so the comparison is
    // apples-to-apples. s3's own golden test (test_s3_two_client_det.cpp)
    // instead uses "largest sim_time anywhere in the trace" as its
    // completion signal, since s3 fully passivates and that file only needs
    // its own single-stage number; that measure includes a real ~0.1s
    // post-completion settling tail (final ACK/state-print activity) that
    // would bias a *cross-stage* delta, so it is deliberately not reused
    // here.
    double completion_time(const std::string &log, std::string_view model_prefix,
                           std::size_t target_count) {
        std::set<int> indices;
        double result = -1.0;
        std::istringstream lines(log);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(model_prefix) == std::string::npos)
                continue;
            double line_sim_time = -1.0;
            {
                constexpr std::string_view time_key = R"("sim_time":)";
                auto pos                            = line.find(time_key);
                if (pos != std::string::npos) {
                    pos += time_key.size();
                    std::size_t end = line.find_first_of(",}", pos);
                    line_sim_time   = std::stod(line.substr(pos, end - pos));
                }
            }
            constexpr std::string_view key = "HAVE idx:";
            std::size_t pos                = 0;
            while ((pos = line.find(key, pos)) != std::string::npos) {
                pos += key.size();
                std::size_t end = pos;
                while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end])))
                    ++end;
                indices.insert(std::stoi(line.substr(pos, end - pos)));
                pos = end;
            }
            if (result < 0.0 && indices.size() >= target_count && line_sim_time >= 0.0) {
                result = line_sim_time;
            }
        }
        return result;
    }

} // namespace

TEST_CASE("s5 det comparison: real policy adds bounded overhead over the stub baseline") {
    static_assert(bt_utp::s3_total_pieces == bt_utp::s4_total_pieces);
    static_assert(bt_utp::s3_sub_pieces_per_piece == bt_utp::s4_sub_pieces_per_piece);
    static_assert(bt_utp::s3_sub_piece_bytes == bt_utp::s4_sub_piece_bytes);
    static_assert(bt_utp::s3_rate_ab == bt_utp::s4_rate_ab);
    static_assert(bt_utp::s3_rate_ba == bt_utp::s4_rate_ba);
    static_assert(bt_utp::s3_prop_delay == bt_utp::s4_prop_delay);

    const auto s3 = bt_utp::run_s3_det();
    const auto s4 = bt_utp::run_s4_det();
    REQUIRE(s3.finish == std::numeric_limits<double>::infinity());
    REQUIRE(s4.finish == 100.0); // s4's own default t_max; never passivates

    const double s3_completion =
        completion_time(s3.ndjson_log, "client_b.wire [", bt_utp::s3_total_pieces);
    const double s4_completion =
        completion_time(s4.ndjson_log, "client_b.wire [", bt_utp::s4_total_pieces);
    REQUIRE(s3_completion >= 0.0);
    REQUIRE(s4_completion >= 0.0);

    // Empirically measured (2026-07-26, this Pi): s3 ~47.44s, s4 ~50.98s --
    // a ~3.5s overhead overall. This is *not* simply "the policy adds a
    // fixed decision-timer delay": the rate series shows s4's transfer
    // doesn't start delivering until ~t=11s (choking_policy's initial 10s
    // rechoke tick gating the first unchoke) versus s3's ~t=1s, a ~10s
    // later start -- yet s4 only ends up ~3.5s behind overall, meaning its
    // active transfer phase is itself faster than s3's stub-driven one
    // (real piece_selector vs. stub_sequential_selector), clawing back
    // roughly 6.5s of that later start. The net overhead could shrink,
    // grow, or in principle invert if either effect's magnitude changes;
    // this is a regression pin on the *current* net result, not a claimed
    // physical lower bound -- 20s is a generous ceiling above the ~3.5s
    // baseline that still catches a genuine multi-x slowdown.
    const double overhead = s4_completion - s3_completion;
    CHECK(overhead >= 0.0);
    CHECK(overhead <= 20.0);

    // Neither client is ever actually choked in this simple seed/leech
    // scenario (both sides stay interested throughout), so the real
    // choking_policy/piece_selector shouldn't change protocol message
    // volume at all versus the stage-3 stubs -- only decision timing.
    for (std::string_view needle : {"REQUEST idx:", "PIECE idx:", "HAVE idx:"}) {
        CHECK(count_occurrences(s3.ndjson_log, needle) == count_occurrences(s4.ndjson_log, needle));
    }
}
