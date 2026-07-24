// SPDX-License-Identifier: BSD-2-Clause
/**
 * Stage-4 deterministic two-client end-to-end exchange: same seed/leech
 * topology as test_s3_two_client_det.cpp, but with the real
 * choking_policy/piece_selector atomics instead of stage-3's stubs
 * (bead e332).
 */
#include "../models/client/s4_two_client_det.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

TEST_CASE("s4 det: smoke test — runs to completion without crashing") {
    const auto result = bt_utp::run_s4_det();
    // Unlike s3_top, choking_policy's rechoke/optimistic timers reschedule
    // themselves every 10s/30s forever, so this system never fully
    // passivates -- run_until()'s return value is always t_max here, never
    // infinity. That is expected (a real running client's choke algorithm
    // keeps ticking as long as it's alive), not a hang; actual transfer
    // completion is checked separately below via the NDJSON trace.
    CHECK(result.finish == 100.0);
    CHECK(!result.ndjson_log.empty());
}

namespace {

    // Non-overlapping occurrences of `needle` in `haystack`.
    std::size_t count_occurrences(std::string_view haystack, std::string_view needle) {
        std::size_t count = 0;
        std::size_t pos   = 0;
        while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    // Distinct piece indices announced via "HAVE idx:N" by `model_prefix`
    // (e.g. "client_b.wire ["), plus the sim_time at which the count first
    // reaches `target_count` distinct indices. Unlike s3's max_sim_time,
    // stage-4's trace keeps accumulating sim_time past actual completion
    // (choking_policy's eternal timers keep ticking), so "largest sim_time
    // in the trace" is meaningless here as a completion signal -- "sim_time
    // of the Nth distinct HAVE" is the thing that actually means "done".
    struct have_progress {
        std::set<int> indices;
        double completion_time = -1.0;
    };

    have_progress track_have_progress(const std::string &log, std::string_view model_prefix,
                                      std::size_t target_count) {
        have_progress result;
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
                result.indices.insert(std::stoi(line.substr(pos, end - pos)));
                pos = end;
            }
            if (result.completion_time < 0.0 && result.indices.size() >= target_count &&
                line_sim_time >= 0.0) {
                result.completion_time = line_sim_time;
            }
        }
        return result;
    }

} // namespace

TEST_CASE("s4 det: golden-value assertions on the filtered NDJSON trace") {
    // One run shared by every assertion below -- see test_s3_two_client_det.cpp
    // for why SECTION would be wasteful here.
    const auto result = bt_utp::run_s4_det();
    REQUIRE(result.finish == 100.0);
    const std::string &log = result.ndjson_log;

    // Completion time (sim_time of the 40th distinct HAVE, not the trace's
    // overall max sim_time -- see track_have_progress's comment) within
    // analytic/regression bounds. Lower bound is the same hard physics
    // guarantee as s3: bulk data can't cross the A->B channel faster than
    // its rate cap.
    constexpr double total_bytes =
        static_cast<double>(bt_utp::s4_total_pieces) * static_cast<double>(bt_utp::s4_piece_bytes);
    const double lower_bound = total_bytes / bt_utp::s4_rate_ab;
    // Upper bound is an empirical regression guard: measured completion is
    // ~51s (slightly above s3's ~47.5s, since choking_policy's initial
    // 10s rechoke tick delays the first unchoke) -- 90s is a generous
    // ceiling that still catches a genuine multi-x slowdown.
    constexpr double upper_bound = 90.0;

    const auto progress = track_have_progress(log, "client_b.wire [", bt_utp::s4_total_pieces);
    CHECK(progress.indices.size() == bt_utp::s4_total_pieces);
    if (!progress.indices.empty()) {
        CHECK(*progress.indices.begin() == 0);
        CHECK(*progress.indices.rbegin() == static_cast<int>(bt_utp::s4_total_pieces) - 1);
    }
    CHECK(progress.completion_time >= lower_bound);
    CHECK(progress.completion_time <= upper_bound);

    // Every REQUEST is answered by exactly one PIECE (see
    // test_s3_two_client_det.cpp for why each raw count is 2x the true
    // sub-piece exchange count -- both sides double identically, so the
    // equality check is unaffected).
    const auto num_requests = count_occurrences(log, "REQUEST idx:");
    const auto num_pieces   = count_occurrences(log, "PIECE idx:");
    CHECK(num_requests > 0);
    CHECK(num_requests == num_pieces);

    // Policy decisions are logged as their own model events, scoped by
    // cadmium::named<> just like every other atomic -- choking_policy's
    // choke/unchoke decisions appear under "client_a.choke [...]" /
    // "client_b.choke [...]", piece_selector's request plans under
    // "client_a.selector [...]" / "client_b.selector [...]". This directly
    // confirms sim_messages_collect already satisfies "choking/selection
    // decisions as their own model events" without any separate mechanism.
    CHECK(log.find("client_a.choke [") != std::string::npos);
    CHECK(log.find("client_b.choke [") != std::string::npos);
    CHECK(log.find("client_b.selector [") != std::string::npos);
}
