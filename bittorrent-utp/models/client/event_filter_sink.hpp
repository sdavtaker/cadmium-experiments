// SPDX-License-Identifier: BSD-2-Clause
/**
 * event_filter_sink: a spdlog sink that forwards to an underlying sink only
 * the lines whose NDJSON "event" field is in a fixed allowlist.
 *
 * cadmium_log.hpp's emit() already writes each line as one complete,
 * single-object NDJSON string with a fixed shape
 * (`{"ts":...,"level":...,"event":"...","msg":...[,"sim_time":...]}`), and
 * the logger's pattern is set to "%v" (payload verbatim, nothing added), so
 * `msg.payload` here already *is* that JSON line — no JSON parser is needed,
 * just finding the fixed `"event":"` key and reading up to the next `"`.
 */
#pragma once

#include <algorithm>
#include <memory>
#include <spdlog/sinks/sink.h>
#include <string>
#include <string_view>
#include <vector>

namespace bt_utp {

    class event_filter_sink : public spdlog::sinks::sink {
      public:
        event_filter_sink(std::shared_ptr<spdlog::sinks::sink> target,
                          std::vector<std::string> allowed_events)
            : target_(std::move(target)), allowed_events_(std::move(allowed_events)) {}

        void log(const spdlog::details::log_msg &msg) override {
            const std::string_view payload(msg.payload.data(), msg.payload.size());
            constexpr std::string_view key = R"("event":")";
            auto pos                       = payload.find(key);
            if (pos == std::string_view::npos)
                return;
            pos += key.size();
            const auto end = payload.find('"', pos);
            if (end == std::string_view::npos)
                return;
            const std::string_view event = payload.substr(pos, end - pos);
            if (std::find(allowed_events_.begin(), allowed_events_.end(), event) !=
                allowed_events_.end())
                target_->log(msg);
        }

        void flush() override {
            target_->flush();
        }

        void set_pattern(const std::string &pattern) override {
            target_->set_pattern(pattern);
        }

        void set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) override {
            target_->set_formatter(std::move(sink_formatter));
        }

      private:
        std::shared_ptr<spdlog::sinks::sink> target_;
        std::vector<std::string> allowed_events_;
    };

} // namespace bt_utp
