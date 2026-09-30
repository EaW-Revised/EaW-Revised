#include "resource_churn_probe.hpp"

// The frames drawn since the step show exactly the expected live instances.
void EawrResourceChurnProbe::expect_drawn(const std::string& step) {
    const Sample now = sample();
    record(step, now);
    check(now.objects == static_cast<std::int64_t>(expected_drawn_), "RenderingServer drew "
        + std::to_string(now.objects) + " objects, expected " + std::to_string(expected_drawn_));
    if (!renderer_) return;
    instance_readback_ = renderer_->instance_evidence();
    check(instance_readback_.size() == expected_instances_, "instance read-back size differs from the registry");
    for (const GodotRenderer::InstanceEvidence& item : instance_readback_) {
        check(item.mesh_surfaces >= 1, "instance " + std::to_string(item.entity_id) + " reads back an empty mesh");
    }
}

std::size_t EawrResourceChurnProbe::count_code(const std::string_view code) const {
    const auto history = renderer_->diagnostics();
    return static_cast<std::size_t>(std::count_if(history.begin(), history.end(),
        [code](const eawr::core::Diagnostic& item) { return item.code == code; }));
}

bool EawrResourceChurnProbe::has_message(const std::string_view text) const {
    const auto history = renderer_->diagnostics();
    return std::any_of(history.begin(), history.end(),
        [text](const eawr::core::Diagnostic& item) { return item.message.find(text) != std::string::npos; });
}

std::vector<std::string> EawrResourceChurnProbe::tail(const std::size_t count) const {
    const auto history = renderer_->diagnostics();
    std::vector<std::string> result;
    for (std::size_t i = history.size() > count ? history.size() - count : 0; i < history.size(); ++i) {
        result.push_back(history[i].code + " " + history[i].message);
    }
    return result;
}

std::size_t EawrResourceChurnProbe::references(const sim::AssetId asset) const {
    for (const presentation::ResourceReference& item : renderer_->resources()) {
        if (item.asset_id == asset) return item.references;
    }
    return 0;
}

const GodotRenderer::InstanceEvidence* EawrResourceChurnProbe::evidence(const sim::EntityId entity) {
    instance_readback_ = renderer_->instance_evidence();
    for (const GodotRenderer::InstanceEvidence& item : instance_readback_) {
        if (item.entity_id == entity) return &item;
    }
    return nullptr;
}

void EawrResourceChurnProbe::finish() {
    finished_ = true;
    check(!renderer_, "renderer destroyed before the report");
    if (leak_control_) {
        // Control only: an unowned mesh RID that is never freed. The runner
        // requires Godot's exit-time leak report for it, which proves that a
        // clean normal run is evidence of no leaked RenderingServer RID.
        static_cast<void>(RenderingServer::get_singleton()->mesh_create());
        evidence_.push_back("\"leak_control\":true");
    }
    const bool passed = failures_.empty();
    std::ostringstream out;
    out << "{\"schema\":\"eawr-resource-churn-probe-v1\",\"status\":\""
        << (passed ? "resource_churn_probe_passed" : "resource_churn_probe_failed") << "\",";
    out << "\"godot_version\":\"" << escape(String(Engine::get_singleton()->get_version_info()["string"]).utf8().get_data())
        << "\",\"rendering_driver\":\""
        << escape(RenderingServer::get_singleton()->get_current_rendering_driver_name().utf8().get_data())
        << "\",\"rendering_method\":\""
        << escape(RenderingServer::get_singleton()->get_current_rendering_method().utf8().get_data())
        << "\",\"adapter\":\"" << escape(RenderingServer::get_singleton()->get_video_adapter_name().utf8().get_data())
        << "\",";
    out << "\"memory\":{\"baseline\":{\"texture\":" << baseline_.texture << ",\"buffer\":" << baseline_.buffer
        << "},\"with_assets\":{\"texture\":" << with_assets_.texture << ",\"buffer\":" << with_assets_.buffer
        << "},\"churn_full\":{\"texture\":" << churn_full_.texture << ",\"buffer\":" << churn_full_.buffer
        << "},\"after_shutdown\":{\"texture\":" << after_shutdown_.texture << ",\"buffer\":" << after_shutdown_.buffer
        << "},\"after_restart\":{\"texture\":" << after_restart_.texture << ",\"buffer\":" << after_restart_.buffer
        << "}},";
    for (const std::string& item : evidence_) out << item << ",";
    out << "\"steps_completed\":" << index_ << ",\"steps_total\":" << steps_.size() << ",\"frames\":" << frames_ << ",";
    out << "\"samples\":[";
    for (std::size_t i = 0; i < samples_.size(); ++i) out << (i ? "," : "") << samples_[i];
    out << "],\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << "\"" << escape(failures_[i]) << "\"";
    out << "]}\n";

    bool written = false;
    if (!report_path_.empty()) {
        std::ofstream file(report_path_, std::ios::binary);
        file << out.str();
        written = static_cast<bool>(file);
    }
    for (const std::string& failure : failures_) UtilityFunctions::print(String("EAWR churn failure: ") + failure.c_str());
    UtilityFunctions::print(String("EAWR resource churn probe ") + (passed ? "passed" : "failed") + " after "
        + String::num_int64(frames_) + " frames");
    get_tree()->quit(passed && written ? 0 : 1);
}
