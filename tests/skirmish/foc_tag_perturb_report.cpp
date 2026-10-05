#include "foc_tag_perturb_support.hpp"

namespace foc_tag_perturb_test_support {


[[nodiscard]] std::string row_json(const RowResult& result) {
    std::ostringstream json;
    json << "{\"id\": " << json_string(result.row.id) << ", \"class\": " << json_string(result.row.element)
         << ", \"tag\": " << json_string(result.row.tag) << ", \"type\": " << json_string(result.row.type)
         << ", \"change\": " << json_string(result.row.change) << ", \"verdict\": " << json_string(result.verdict)
         << ", \"reason\": " << json_string(result.reason) << ", \"objects\": [";
    for (std::size_t index = 0; index < result.objects.size(); ++index) {
        const auto& object = result.objects[index];
        const auto list = [](const std::vector<std::string>& values) {
            std::string text = "[";
            for (std::size_t at = 0; at < values.size(); ++at) text += (at ? ", " : "") + json_string(values[at]);
            return text + "]";
        };
        json << (index ? ", " : "") << "{\"id\": " << json_string(object.id) << ", \"from\": " << list(object.from)
             << ", \"to\": " << list(object.to) << ", \"read\": " << (object.traced ? (object.read ? "true" : "false") : "null") << "}";
    }
    json << "], \"without_tag\": " << result.skipped.size() << ", \"tables_changed\": "
         << (result.tables_changed ? "true" : "false") << ", \"first_tick\": "
         << (result.first_tick ? std::to_string(*result.first_tick) : std::string("null")) << ", \"kinds\": {";
    bool first = true;
    for (const auto& [kind, delta] : result.kinds) {
        if (!delta.first_tick) continue;
        json << (first ? "" : ", ") << json_string(kind) << ": {\"first_tick\": " << *delta.first_tick
             << ", \"units\": " << delta.units << ", \"max_position_delta\": " << json_number(delta.position)
             << ", \"max_height_delta\": " << json_number(delta.height) << "}";
        first = false;
    }
    json << "}, \"ticks_run\": " << result.ticks_run << ", \"workers_check\": "
         << (result.workers_check.empty() ? std::string("null") : json_string(result.workers_check))
         << ", \"seconds\": " << json_number(result.seconds) << "}";
    return json.str();
}

} // namespace foc_tag_perturb_test_support
