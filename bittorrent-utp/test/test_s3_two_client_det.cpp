// SPDX-License-Identifier: BSD-2-Clause
/**
 * Deterministic two-client end-to-end exchange: clientA (seed, full
 * bitfield) transfers all 40 pieces to clientB (empty) over two real
 * bottleneck_channel instances, driving two real bittorrent_client
 * couplings through cadmium's own runner (no hand-rolled scheduler).
 */
#include "../models/client/s3_two_client_det.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

TEST_CASE("s3 det: smoke test — runs to completion without crashing") {
    const auto result = bt_utp::run_s3_det();
    // runner.run_until() returns the *next scheduled* time, not the last
    // processed one — once the transfer completes and both clients settle
    // (A never requests anything as a full seed; B stops once it has
    // everything, per peer_wire's own-completion interest recompute),
    // nothing remains scheduled and this correctly becomes infinity. That
    // is the actual "done, no hang" signal here, not a failure to finish
    // within the time budget.
    CHECK(result.finish == std::numeric_limits<double>::infinity());
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

    // Largest "sim_time":<number> value in the (NDJSON, one object per
    // line) trace -- a stand-in for "when did the last thing happen",
    // since run_s3_det()'s own finish value is only ever infinity (the
    // *next scheduled* time) or a t_max cutoff, never the actual moment
    // the transfer completed.
    double max_sim_time(const std::string &log) {
        constexpr std::string_view key = R"("sim_time":)";
        double result                  = 0.0;
        std::size_t pos                = 0;
        while ((pos = log.find(key, pos)) != std::string_view::npos) {
            pos += key.size();
            std::size_t end = log.find_first_of(",}", pos);
            result          = std::max(result, std::stod(log.substr(pos, end - pos)));
            pos             = end;
        }
        return result;
    }

    // Distinct piece indices in "HAVE idx:N" occurrences on lines whose
    // model name is `model_prefix` (e.g. "client_b.wire [") -- scoping by
    // model name (available thanks to cadmium::named<>) is what lets this
    // tell "clientB announcing a piece it just finished downloading" apart
    // from any other HAVE-shaped text elsewhere in the trace.
    std::set<int> have_indices_from(const std::string &log, std::string_view model_prefix) {
        std::set<int> indices;
        std::istringstream lines(log);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(model_prefix) == std::string::npos)
                continue;
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
        }
        return indices;
    }

} // namespace

TEST_CASE("s3 det: golden-value assertions on the filtered NDJSON trace") {
    // One run shared by every assertion below -- Catch2's SECTION re-runs
    // the whole TEST_CASE body per leaf section, which would triple the
    // cost of this (expensive) simulation run for no benefit here.
    const auto result = bt_utp::run_s3_det();
    REQUIRE(result.finish == std::numeric_limits<double>::infinity());
    const std::string &log = result.ndjson_log;

    // Completion time within analytic/regression bounds. Lower bound is a
    // hard physics guarantee: the bulk data (every piece, once) cannot
    // cross the A->B channel faster than its rate cap allows, regardless
    // of protocol behavior.
    constexpr double total_bytes =
        static_cast<double>(bt_utp::s3_total_pieces) * static_cast<double>(bt_utp::s3_piece_bytes);
    const double lower_bound = total_bytes / bt_utp::s3_rate_ab;
    // Upper bound is an empirical regression guard, not a derived formula:
    // measured completion is ~47.5s (uTP/LEDBAT congestion control, the
    // request pipeline, and per-frame protocol overhead all push it well
    // above the bytes/rate floor above) -- 100s is a generous ceiling that
    // still catches a genuine multi-x slowdown.
    constexpr double upper_bound = 100.0;
    const double finish_time     = max_sim_time(log);
    CHECK(finish_time >= lower_bound);
    CHECK(finish_time <= upper_bound);

    // Every REQUEST is answered by exactly one PIECE.
    const auto num_requests = count_occurrences(log, "REQUEST idx:");
    const auto num_pieces   = count_occurrences(log, "PIECE idx:");
    CHECK(num_requests > 0);
    CHECK(num_requests == num_pieces);

    // clientB announces exactly the 40 distinct pieces it completes.
    // clientA is a full seed from construction and never completes a
    // download, so it never emits HAVE -- only clientB (the leech) does,
    // exactly once per piece as it finishes downloading it.
    const auto have_indices = have_indices_from(log, "client_b.wire [");
    CHECK(have_indices.size() == bt_utp::s3_total_pieces);
    if (!have_indices.empty()) {
        CHECK(*have_indices.begin() == 0);
        CHECK(*have_indices.rbegin() == static_cast<int>(bt_utp::s3_total_pieces) - 1);
    }
}
