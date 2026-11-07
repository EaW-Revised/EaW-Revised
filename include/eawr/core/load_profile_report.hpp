#pragma once

#include "eawr/core/json_text.hpp"
#include "eawr/core/load_profile.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <iomanip>
#include <ostream>
#include <string_view>

namespace eawr::core::load_profile {

// The load-check attribution report (opt-in, written only when a Recording was active): per-phase totals with the
// slowest single call and the threads that ran it, the per-layer XML cost, the `top_files` most expensive files, and
// every path requested more than once. `extra_members` is a comma-free list of already formatted JSON members
// ("\"k\":v,\"k2\":v2") the caller adds at the top level. Presentation diagnostics only.
inline void write_report(std::ostream& output, const Recording& recording, const std::size_t top_files,
                         const std::string_view extra_members = {}) {
    // Fixed notation, so large millisecond totals keep their decimals instead of switching to 6 significant digits.
    const auto saved_flags = output.flags();
    const auto saved_precision = output.precision();
    output << std::fixed << std::setprecision(3);
    output << "{\"schema\":\"eawr.load_profile/1\",\"recorder_scope\":\"calling-thread-only\"";
    if (!extra_members.empty()) output << ',' << extra_members;
    output << ",\"phases\":{";
    bool first = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& cost = recording.costs[i];
        if (cost.calls == 0) continue;
        output << (first ? "" : ",") << '"' << names[i] << "\":{\"calls\":" << cost.calls
               << ",\"exclusive_ms\":" << cost.exclusive_ms << ",\"inclusive_ms\":" << cost.inclusive_ms
               << ",\"max_ms\":" << cost.max_ms << ",\"threads\":[";
        for (std::size_t t = 0; t < recording.threads[i].size(); ++t)
            output << (t ? "," : "") << recording.threads[i][t];
        output << "]}";
        first = false;
    }
    output << "},\"layers\":";
    write_layer_costs(output, recording);
    std::vector<std::size_t> order(recording.files.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
        return recording.files[a].total_ms() > recording.files[b].total_ms();
    });
    output << ",\"files\":{\"count\":" << recording.files.size() << ",\"top\":[";
    for (std::size_t i = 0; i < order.size() && i < top_files; ++i) {
        const auto& file = recording.files[order[i]];
        output << (i ? "," : "") << "{\"path\":" << json_quote(file.path) << ",\"layer\":" << json_quote(file.layer)
               << ",\"bytes\":" << file.bytes << ",\"tags\":" << file.tags << ",\"includes\":" << file.includes
               << ",\"requests\":" << file.requests << ",\"read_ms\":" << file.read_ms
               << ",\"parse_ms\":" << file.parse_ms << ",\"merge_ms\":" << file.merge_ms
               << ",\"total_ms\":" << file.total_ms() << '}';
    }
    output << "]},\"counters\":{";
    for (std::size_t i = 0; i < counter_names.size(); ++i)
        output << (i ? "," : "") << '"' << counter_names[i] << "\":" << recording.counters[i];
    output << "},\"resolve_depth\":[";
    first = true;
    for (std::size_t depth = 0; depth < recording.resolve_depth.size(); ++depth) {
        const auto& bucket = recording.resolve_depth[depth];
        if (bucket.calls == 0) continue;
        output << (first ? "" : ",") << "{\"depth\":" << depth << ",\"calls\":" << bucket.calls
               << ",\"ms\":" << bucket.ms << '}';
        first = false;
    }
    std::vector<std::size_t> models(recording.models.size());
    std::iota(models.begin(), models.end(), std::size_t{0});
    std::stable_sort(models.begin(), models.end(), [&](const std::size_t a, const std::size_t b) {
        return recording.models[a].total_ms() > recording.models[b].total_ms();
    });
    double decode_ms = 0, sha_ms = 0, bind_ms = 0, read_ms = 0, parse_ms = 0;
    std::uint64_t file_bytes = 0, io_read_bytes = 0;
    for (const auto& model : recording.models) {
        decode_ms += model.decode_ms; sha_ms += model.sha_ms; bind_ms += model.bind_ms;
        read_ms += model.read_ms; parse_ms += model.parse_ms;
        file_bytes += model.file_bytes; io_read_bytes += model.io_read_bytes;
    }
    output << "],\"models\":{\"count\":" << recording.models.size() << ",\"decode_ms\":" << decode_ms
           << ",\"sha_ms\":" << sha_ms << ",\"bind_ms\":" << bind_ms << ",\"read_ms\":" << read_ms
           << ",\"parse_ms\":" << parse_ms << ",\"file_bytes\":" << file_bytes << ",\"io_read_bytes\":" << io_read_bytes
           << ",\"top\":[";
    for (std::size_t i = 0; i < models.size() && i < top_files; ++i) {
        const auto& model = recording.models[models[i]];
        output << (i ? "," : "") << "{\"path\":" << json_quote(model.path) << ",\"bytes\":" << model.bytes
               << ",\"requests\":" << model.requests << ",\"loads\":" << model.loads
               << ",\"decode_ms\":" << model.decode_ms << ",\"sha_ms\":" << model.sha_ms
               << ",\"bind_ms\":" << model.bind_ms << ",\"total_ms\":" << model.total_ms()
               << ",\"read_ms\":" << model.read_ms << ",\"parse_ms\":" << model.parse_ms
               << ",\"file_bytes\":" << model.file_bytes << ",\"io_read_bytes\":" << model.io_read_bytes << '}';
    }
    output << "]},\"io_read_bytes\":{";
    first = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (recording.io_read_bytes[i] == 0) continue;
        output << (first ? "" : ",") << '"' << names[i] << "\":" << recording.io_read_bytes[i];
        first = false;
    }
    output << "},\"repeated_includes\":[";
    first = true;
    for (const auto& file : recording.files) {
        if (file.requests < 2) continue;
        output << (first ? "" : ",") << "{\"path\":" << json_quote(file.path) << ",\"requests\":" << file.requests << '}';
        first = false;
    }
    output << "]}";
    output.flags(saved_flags);
    output.precision(saved_precision);
}

} // namespace eawr::core::load_profile
