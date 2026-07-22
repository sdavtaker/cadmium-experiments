// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for event_filter_sink: does it forward only lines whose
 * "event" field is in the allowlist, and does it fail safe (drop, not
 * crash) on a line with no "event" field at all.
 */
#include "../models/client/event_filter_sink.hpp"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <vector>

namespace {

    std::string emit_and_capture(const std::vector<std::string> &allowlist,
                                 const std::vector<std::string> &lines) {
        auto buffer   = std::make_shared<std::ostringstream>();
        auto raw_sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*buffer);
        auto sink     = std::make_shared<bt_utp::event_filter_sink>(raw_sink, allowlist);
        auto logger   = std::make_shared<spdlog::logger>("test_event_filter_sink", sink);
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::debug);
        for (const auto &line : lines)
            logger->debug(line);
        logger->flush();
        return buffer->str();
    }

} // namespace

TEST_CASE("event_filter_sink forwards lines whose event is in the allowlist") {
    const auto out = emit_and_capture(
        {"sim_messages_collect"},
        {R"({"ts":"t","level":"debug","event":"sim_messages_collect","msg":"kept"})"});
    CHECK(out.find("kept") != std::string::npos);
}

TEST_CASE("event_filter_sink drops lines whose event is not in the allowlist") {
    const auto out =
        emit_and_capture({"sim_messages_collect"},
                         {R"({"ts":"t","level":"debug","event":"sim_state","msg":"dropped"})"});
    CHECK(out.find("dropped") == std::string::npos);
    CHECK(out.empty());
}

TEST_CASE("event_filter_sink filters a mixed batch, keeping only allowed events") {
    const auto out = emit_and_capture(
        {"sim_messages_collect", "run_global_time"},
        {R"({"ts":"t","level":"debug","event":"sim_state","msg":"noise1"})",
         R"({"ts":"t","level":"debug","event":"sim_messages_collect","msg":"keep1"})",
         R"({"ts":"t","level":"debug","event":"coor_info_advance","msg":"noise2"})",
         R"({"ts":"t","level":"debug","event":"run_global_time","msg":"keep2"})"});
    CHECK(out.find("keep1") != std::string::npos);
    CHECK(out.find("keep2") != std::string::npos);
    CHECK(out.find("noise1") == std::string::npos);
    CHECK(out.find("noise2") == std::string::npos);
}

TEST_CASE("event_filter_sink drops a line with no event field rather than crashing") {
    const auto out = emit_and_capture(
        {"sim_messages_collect"}, {R"({"ts":"t","level":"debug","msg":"no event field here"})"});
    CHECK(out.empty());
}
