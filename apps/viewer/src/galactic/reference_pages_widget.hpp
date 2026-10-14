#pragma once
#include "galactic_mode.hpp"
#include "reference_catalog.hpp"
#include "ui/dialog_builder.hpp"
#include "ui/input_routing.hpp"

namespace eawr::presentation::godot_backend {
class ReferencePages final {
public:
    void service(GalacticMode::State&);
private:
    viewer::galactic::ReferencePagesModel model_;
    godot::Control* root_{};
    std::optional<BuiltDialog> dialog_;
    std::function<BuiltDialog(std::string_view, godot::Vector2)> build_;
    std::shared_ptr<data::ui::DialogCatalog> catalog_;
    std::optional<viewer::galactic::ReferenceCatalog> reference_catalog_;
    std::function<std::string(std::string_view)> text_;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> snapshot_;
    godot::Vector2 size_;
    std::string dialog_name_;
    godot::Vector2 dialog_size_;
    std::map<std::uint64_t, viewer::galactic::ReferenceType> types_;
    std::map<viewer::galactic::ObjectId, viewer::galactic::ReferenceType> planets_;
    std::optional<viewer::galactic::TreeFacts> authored_tree_;
    bool dirty_{true};
    void rebuild(GalacticMode::State&);
    void refresh(GalacticMode::State&);
    void activate(GalacticMode::State&, const std::string&);
    [[nodiscard]] viewer::galactic::ReferenceFacts facts(GalacticMode::State&);
};
void service_reference_pages(GalacticMode::State&);
} // namespace eawr::presentation::godot_backend
