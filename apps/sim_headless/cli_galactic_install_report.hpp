#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/json_text.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sim_headless::cli {

// One line per stage on stdout and one row in the JSON report, including the
// diagnostic that stopped the run and any script faults collected before it.
class InstalledCampaignReport {
public:
    explicit InstalledCampaignReport(std::ostream& output = std::cout) : output_(output) {}

    void note(const std::string& stage, const bool ok, const std::string& detail) {
        output_ << (ok ? "[ok]   " : "[FAIL] ") << stage << ": " << detail << '\n';
        rows_.push_back({stage, ok, detail});
    }
    void fact(const std::string& key, const std::string& value) { facts_.emplace_back(key, value); }

    void live_failure(const std::optional<eawr::core::Diagnostic>& failure, const bool reached,
        const bool timed_out, const std::uint64_t requested, const std::uint64_t completed) {
        const auto progress = "tick " + std::to_string(requested) + " (at " + std::to_string(completed) + ")";
        if (failure) note("live-run", false, "the live session failed before " + progress + ": "
            + eawr::core::format_diagnostic(*failure));
        else if (!reached) note("live-run", false, (timed_out ? "timed out waiting for live session "
            : "the live session ended before ") + progress);
        else note("live-run", false, "the live session reached " + progress + " without a published snapshot");
    }

    void story_scripts(const std::map<std::string, std::size_t>& faults) {
        std::ostringstream rows;
        rows << '[';
        std::size_t index = 0;
        for (const auto& [text, count] : faults)
            rows << (index++ ? ",\n      " : "\n      ")
                 << eawr::core::json_quote(text + " (x" + std::to_string(count) + ")");
        rows << "\n    ]";
        fact("story_script_faults", rows.str());
        note("story-scripts", true, std::to_string(faults.size()) + " distinct sub-plot script faults");
        for (const auto& [text, count] : faults) output_ << "       " << text << " (x" << count << ")\n";
    }

    [[nodiscard]] bool write(const std::string& path) const {
        std::ostringstream json;
        json << "{\n  \"stages\": [";
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            json << (i ? "," : "") << "\n    {\"stage\": " << eawr::core::json_quote(rows_[i].stage) << ", \"ok\": "
                 << (rows_[i].ok ? "true" : "false") << ", \"detail\": " << eawr::core::json_quote(rows_[i].detail) << '}';
        }
        json << "\n  ],\n  \"facts\": {";
        for (std::size_t i = 0; i < facts_.size(); ++i) {
            json << (i ? "," : "") << "\n    " << eawr::core::json_quote(facts_[i].first) << ": " << facts_[i].second;
        }
        json << "\n  }\n}\n";
        std::ofstream out(path, std::ios::binary);
        out << json.str();
        return static_cast<bool>(out);
    }

private:
    struct Row {
        std::string stage;
        bool ok;
        std::string detail;
    };
    std::ostream& output_;
    std::vector<Row> rows_;
    std::vector<std::pair<std::string, std::string>> facts_;
};

} // namespace sim_headless::cli
