#include "fog_renderer_probe.hpp"

void EawrFogRendererProbe::record_stage(const std::string& name) {
    const auto status = renderer_ ? renderer_->fog_status() : GodotRenderer::FogStatus{};
    const auto& s = status.cache;
    std::ostringstream out;
    out << "{\"stage\":\"" << escape(name) << "\",\"readiness\":\"" << readiness_name(status.readiness) << "\""
        << ",\"team\":" << (status.selection ? std::to_string(status.selection->team) : std::string("null"))
        << ",\"stream\":" << (status.selection ? std::to_string(status.selection->stream) : std::string("null"))
        << ",\"bound_revision\":" << (status.bound_revision ? std::to_string(*status.bound_revision) : std::string("null"))
        << ",\"submitted_tick\":" << (status.submitted_tick ? std::to_string(*status.submitted_tick) : std::string("null"))
        << ",\"last_action\":" << (status.last_action ? "\"" + std::string(fog::to_string(*status.last_action)) + "\"" : std::string("null"))
        << ",\"rejection\":" << (status.last_rejection ? "\"" + status.last_rejection->code + "\"" : std::string("null"))
        << ",\"rejection_message\":" << (status.last_rejection ? "\"" + escape(status.last_rejection->message) + "\"" : std::string("null"))
        << ",\"binding_retained\":" << (status.binding_retained ? "true" : "false")
        << ",\"uploads\":" << s.uploads << ",\"upload_bytes\":" << s.upload_bytes << ",\"creates\":" << s.creates
        << ",\"recreates\":" << s.recreates << ",\"updates\":" << s.updates << ",\"destroys\":" << s.destroys
        << ",\"binds\":" << s.binds << ",\"unbinds\":" << s.unbinds << ",\"metadata_only\":" << s.metadata_only
        << ",\"reselects\":" << s.reselects << ",\"rejected\":" << s.rejected
        << ",\"live_textures\":" << status.live_textures << ",\"attached\":" << status.attached_consumers
        << ",\"declared\":" << status.declared_consumers << ",\"unsupported\":" << status.unsupported.size() << "}";
    stages_.push_back(out.str());
}

std::vector<std::pair<std::string, std::string>> EawrFogRendererProbe::fog_parameters(const sim::AssetId asset) {
    for (const auto& consumer : renderer_->fog_consumers()) {
        if (consumer.asset_id == asset) return consumer.fog_parameters;
    }
    check(false, "asset " + std::to_string(asset) + " is not a declared fog consumer");
    return {};
}

void EawrFogRendererProbe::record_parameters(const std::string& label, const sim::AssetId asset) {
    std::string list = "{\"label\":\"" + escape(label) + "\",\"asset\":" + std::to_string(asset) + ",\"parameters\":{";
    bool first = true;
    for (const auto& [name, value] : fog_parameters(asset)) {
        list += (first ? "\"" : ",\"") + escape(name) + "\":\"" + escape(value) + "\"";
        first = false;
    }
    parameter_evidence_.push_back(list + "}}");
}

PackedByteArray EawrFogRendererProbe::grab() {
    Ref<Image> image = viewport_->get_texture()->get_image();
    check(image.is_valid() && image->get_width() == view_width && image->get_height() == view_height,
          "viewport capture size");
    if (!image.is_valid()) return {};
    image->convert(Image::FORMAT_RGBA8);
    return image->get_data();
}

void EawrFogRendererProbe::finish() {
    finished_ = true;
    check(!renderer_, "renderer destroyed before the report");
    const bool passed = failures_.empty();
    std::ostringstream out;
    out << "{\"schema\":\"eawr-fog-renderer-probe-v1\",\"status\":\""
        << (failures_.empty() ? "fog_renderer_probe_passed" : "fog_renderer_probe_failed") << "\",";
    out << "\"godot_version\":\"" << escape(String(Engine::get_singleton()->get_version_info()["string"]).utf8().get_data())
        << "\",\"rendering_driver\":\""
        << escape(RenderingServer::get_singleton()->get_current_rendering_driver_name().utf8().get_data())
        << "\",\"rendering_method\":\""
        << escape(RenderingServer::get_singleton()->get_current_rendering_method().utf8().get_data()) << "\",";
    out << "\"view\":{\"width\":" << view_width << ",\"height\":" << view_height << ",\"fov_degrees\":" << fov_degrees << "},";
    out << "\"texture_memory\":{\"baseline\":" << texture_memory_baseline_ << ",\"with_assets\":" << texture_memory_with_assets_
        << ",\"before_enable\":" << texture_memory_before_enable_ << ",\"with_fog\":" << texture_memory_with_fog_ << ",\"after_disable\":" << texture_memory_after_disable_
        << ",\"after_destroy\":" << texture_memory_after_destroy_ << "},";
    out << "\"destroyed_with_fog_enabled\":" << (destroyed_with_fog_enabled_ ? "true" : "false") << ",";
    for (const std::string& item : evidence_) out << item << ",";
    out << "\"fog_parameters\":[";
    for (std::size_t i = 0; i < parameter_evidence_.size(); ++i) out << (i ? "," : "") << parameter_evidence_[i];
    out << "],";
    out << "\"steps_completed\":" << index_ << ",\"steps_total\":" << steps_.size() << ",\"frames\":" << frames_ << ",";
    out << "\"stages\":[";
    for (std::size_t i = 0; i < stages_.size(); ++i) out << (i ? "," : "") << stages_[i];
    out << "],\"renders\":[";
    for (std::size_t i = 0; i < renders_.size(); ++i) out << (i ? "," : "") << renders_[i];
    out << "],\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << "\"" << escape(failures_[i]) << "\"";
    out << "]}\n";

    bool written = false;
    if (!report_path_.empty()) {
        std::ofstream file(report_path_, std::ios::binary);
        file << out.str();
        written = static_cast<bool>(file);
    }
    UtilityFunctions::print(String("EAWR fog renderer probe ") + (passed ? "passed" : "failed") + " after "
        + String::num_int64(frames_) + " frames");
    get_tree()->quit(passed && written ? 0 : 1);
}
