// SPDX-License-Identifier: BSD-2-Clause
/**
 * Anti-snub scenario: two clients with complementary partial bitfields
 * (models/client/s4_anti_snub_det.hpp), one direction (clientB's uploads
 * to clientA) deliberately stalled by a permanently-slow channel. Verifies
 * choking_policy's snub mechanism (bead e332) produces its real,
 * observable effect -- a CHOKE command for the non-reciprocating peer --
 * even though `snubbed` itself is pure internal state never emitted as
 * its own event.
 */
#include "../models/client/s4_anti_snub_det.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

TEST_CASE("s4 anti-snub det: smoke test — runs to completion without crashing") {
    const auto result = bt_utp::run_s4as_det();
    // Same eternal-timer situation as s4_two_client_det.hpp's smoke test:
    // choking_policy's rechoke/optimistic timers never let this system
    // fully passivate, so finish is always t_max, never infinity.
    CHECK(result.finish == 100.0);
    CHECK(!result.ndjson_log.empty());
}

namespace {

    // sim_time of the first line attributed to `model_prefix` containing
    // `needle`, or nullopt if it never appears.
    std::optional<double> find_first_sim_time(const std::string &log, std::string_view model_prefix,
                                              std::string_view needle) {
        std::istringstream lines(log);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(model_prefix) == std::string::npos)
                continue;
            if (line.find(needle) == std::string::npos)
                continue;
            constexpr std::string_view time_key = R"("sim_time":)";
            auto pos                            = line.find(time_key);
            if (pos == std::string::npos)
                continue;
            pos += time_key.size();
            std::size_t end = line.find_first_of(",}", pos);
            return std::stod(line.substr(pos, end - pos));
        }
        return std::nullopt;
    }

} // namespace

TEST_CASE("s4 anti-snub det: clientA snubs and chokes clientB after the stalled reciprocity "
          "window, clientB never snubs clientA") {
    // One run shared by every assertion below -- see test_s3_two_client_det.cpp
    // for why SECTION would be wasteful here.
    const auto result = bt_utp::run_s4as_det();
    REQUIRE(result.finish == 100.0);
    const std::string &log = result.ndjson_log;

    // clientA unchokes clientB (peer id 2) quickly: clientB is clientA's
    // only interested peer, so it trivially ranks into the top-4 rate
    // slots at clientA's very first rechoke tick (period 10s).
    const auto unchoke_time = find_first_sim_time(log, "client_a.choke [", "{UNCHOKE 2}");
    REQUIRE(unchoke_time.has_value());
    CHECK(*unchoke_time <= 15.0);

    // chan_ba (clientB's uploads to clientA) is deliberately far too slow
    // for any data to arrive within snub_seconds of that unchoke, so
    // clientA's rechoke() marks clientB snubbed and issues a CHOKE for it
    // -- this is `snubbed`'s only observable effect, since the flag
    // itself is never emitted as its own event (see choking_policy.hpp's
    // rechoke()). The snub check only runs on clientA's fixed 10s rechoke
    // grid, so the very first tick where the condition holds is exactly
    // unchoke_time + snub_seconds (both are exact multiples of the same
    // 10s grid).
    const auto choke_time = find_first_sim_time(log, "client_a.choke [", "{CHOKE 2}");
    REQUIRE(choke_time.has_value());
    CHECK(std::abs(*choke_time - (*unchoke_time + bt_utp::choking_policy<double>::snub_seconds)) <
          1e-6);

    // clientB, by contrast, never snubs clientA: chan_ab (clientA's
    // uploads to clientB) is fast and unstalled, so clientB keeps
    // receiving data from clientA well within the reciprocity window.
    // clientB does still unchoke clientA once (confirming the reciprocal
    // relationship is genuinely in play, not just absent), but never
    // chokes it afterward.
    CHECK(find_first_sim_time(log, "client_b.choke [", "{UNCHOKE 1}").has_value());
    CHECK_FALSE(find_first_sim_time(log, "client_b.choke [", "{CHOKE 1}").has_value());
}
