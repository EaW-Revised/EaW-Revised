// Scripted pointer steps and the scene probe for the lobby harness (#5512). The other scripted steps call a
// button's handler; these push real mouse events through the viewport, so the GUI's own hit test decides
// what is reached, as it does for a player's click. The probe reports what the GUI sees: the hovered
// control, every visible modal layer with its canvas layer index, and the saved-game list's state.

#include "lan_lobby_screen_internal.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>

#include <sstream>

namespace eawr::presentation::godot_backend {
namespace {
constexpr const char* load_list_gadget = "IDC_SAVE_GAME_LIST";
constexpr const char* load_prefix = "load:";

std::vector<std::string>& pointer_log() {
    static std::vector<std::string> log;
    return log;
}

std::string json_text(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) { out += ' '; continue; }
        out += c;
    }
    return out + "\"";
}

std::string path_of(const Node* node) {
    return node != nullptr && node->is_inside_tree() ? utf8(String(node->get_path())) : std::string{};
}

const char* yes_no(const bool value) { return value ? "true" : "false"; }

int layer_of(const Node* node) {
    for (const Node* walk = node; walk != nullptr; walk = walk->get_parent())
        if (const auto* layer = Object::cast_to<CanvasLayer>(walk)) return layer->get_layer();
    return 0;
}

std::size_t visible_content(Node* node) {
    std::size_t count = 0;
    for (int32_t i = 0; i < node->get_child_count(); ++i) {
        Node* child = node->get_child(i);
        if (const auto* control = Object::cast_to<Control>(child))
            if (control->is_visible_in_tree() && Object::cast_to<ColorRect>(child) == nullptr) ++count;
        count += visible_content(child);
    }
    return count;
}

// Every EawrUiModalLayer in the tree, in tree order: where it sits (canvas layer index) and whether it is
// visible with visible content (a dialog) in it.
void collect_modals(Node* node, std::size_t& order, std::vector<std::string>& out) {
    if (const auto* modal = Object::cast_to<EawrUiModalLayer>(node)) {
        std::ostringstream item;
        item << "{\"path\": " << json_text(path_of(modal)) << ", \"layer\": " << layer_of(modal) << ", \"order\": " << order
             << ", \"visible\": " << yes_no(modal->is_visible_in_tree()) << ", \"content\": " << visible_content(node) << "}";
        out.push_back(item.str());
    }
    ++order;
    for (int32_t i = 0; i < node->get_child_count(); ++i) collect_modals(node->get_child(i), order, out);
}

void push_pointer(Viewport& viewport, const Vector2 at, const int phase) {
    if (phase == 0) {
        Ref<InputEventMouseMotion> motion;
        motion.instantiate();
        motion->set_position(at);
        motion->set_global_position(at);
        viewport.push_input(motion, true);
        return;
    }
    Ref<InputEventMouseButton> button;
    button.instantiate();
    button->set_position(at);
    button->set_global_position(at);
    button->set_button_index(MOUSE_BUTTON_LEFT);
    button->set_pressed(phase == 1);
    if (phase == 1) button->set_button_mask(MOUSE_BUTTON_MASK_LEFT);
    viewport.push_input(button, true);
}
} // namespace

bool EawrLanLobby::run_pointer_action(const std::string& verb, const std::string& argument) {
    if (verb != "hover" && verb != "click" && verb != "click_row") return false;
    Impl& impl = *impl_;
    Viewport* viewport = get_viewport();
    if (viewport == nullptr) return false;
    // hover:<id>  click:<id>  (an id of the lobby page, or load:<id> in the saved-game list)  click_row:<n>.
    // A missing or hidden target is a recorded result, not a failed step: the case asserts on it.
    Control* target = nullptr;
    std::optional<Vector2> at;
    if (verb == "click_row") {
        auto* list = impl.list() != nullptr ? Object::cast_to<ItemList>(impl.list()->gadget(load_list_gadget)) : nullptr;
        std::uint32_t row{};
        const bool parsed = !argument.empty() && std::from_chars(argument.data(), argument.data() + argument.size(), row).ec == std::errc{};
        if (list != nullptr && parsed && static_cast<int>(row) < list->get_item_count() && list->is_visible_in_tree()) {
            const Rect2 rect = list->get_item_rect(static_cast<int>(row));
            const float scroll = list->get_v_scroll_bar() != nullptr ? static_cast<float>(list->get_v_scroll_bar()->get_value()) : 0.0F;
            at = list->get_global_transform_with_canvas().xform(Vector2(rect.position.x + rect.size.x * 0.5F, rect.position.y - scroll + rect.size.y * 0.5F));
            target = list;
        }
    } else if (argument.rfind(load_prefix, 0) == 0) {
        if (impl.list() != nullptr) target = impl.list()->gadget(argument.substr(std::string_view(load_prefix).size()));
    } else if (state_ != nullptr && state_->controls.count(argument) != 0) {
        target = state_->controls.at(argument);
    }
    if (target != nullptr && !at && target->is_visible_in_tree())
        at = target->get_global_transform_with_canvas().xform(target->get_size() * 0.5F);
    std::ostringstream entry;
    entry << "{\"verb\": " << json_text(verb) << ", \"target\": " << json_text(argument) << ", \"found\": " << yes_no(target != nullptr)
          << ", \"visible\": " << yes_no(target != nullptr && target->is_visible_in_tree());
    if (const auto* button = Object::cast_to<BaseButton>(target)) entry << ", \"disabled\": " << yes_no(button->is_disabled());
    if (at) {
        push_pointer(*viewport, *at, 0);
        Control* hovered = viewport->gui_get_hovered_control();
        entry << ", \"at\": [" << at->x << ", " << at->y << "], \"hovered\": " << json_text(path_of(hovered)) << ", \"reaches\": "
              << yes_no(hovered != nullptr && (hovered == target || target->is_ancestor_of(hovered)));
        if (verb != "hover") {
            push_pointer(*viewport, *at, 1);
            push_pointer(*viewport, *at, 2);
        }
    }
    entry << "}";
    pointer_log().push_back(entry.str());
    return true;
}

std::string EawrLanLobby::pointer_json() const {
    std::ostringstream out;
    out << "{\"log\": [";
    const auto& log = pointer_log();
    for (std::size_t i = 0; i < log.size(); ++i) out << (i ? ", " : "") << log[i];
    out << "]";
    if (!impl_ || !state_) return out.str() + "}";
    const Impl& impl = *impl_;
    const Viewport* viewport = get_viewport();
    out << ", \"hovered\": " << json_text(path_of(viewport != nullptr ? viewport->gui_get_hovered_control() : nullptr));
    out << ", \"lobby_layer\": " << get_layer();
    out << ", \"listing_pending\": " << yes_no(impl.checkpoint_listing.valid());
    out << ", \"load_pending\": " << yes_no(impl.load_pending);
    out << ", \"problem\": " << json_text(impl.checkpoint_problem);
    const auto* load_game = EAWR_CONTROL(EawrUiButton, "EAWR_GC_LOAD_GAME");
    out << ", \"load_game_disabled\": " << (load_game != nullptr ? yes_no(load_game->is_disabled()) : "null");
    const auto* header = EAWR_CONTROL(EawrUiLabel, "IDC_INTERNET_DIALOG_HEADER");
    out << ", \"header\": " << json_text(header != nullptr ? utf8(header->get_text()) : std::string{});
    out << ", \"title\": " << json_text(utf8(resolved(resources_->text, impl.host ? "TEXT_INTERNET_HOST_GAME_TITLE" : "TEXT_INTERNET_JOIN_GAME_TITLE")));
    out << ", \"load_list\": {\"open\": " << yes_no(impl.list() != nullptr);
    if (impl.list() != nullptr) {
        auto* list = Object::cast_to<ItemList>(impl.list()->gadget(load_list_gadget));
        const auto selected = list != nullptr ? list->get_selected_items() : PackedInt32Array();
        out << ", \"layer\": " << impl.list()->get_layer() << ", \"in_tree\": " << yes_no(impl.list()->is_inside_tree())
            << ", \"visible\": " << yes_no(impl.list()->frame() != nullptr && impl.list()->frame()->is_visible_in_tree())
            << ", \"rows\": " << (list != nullptr ? list->get_item_count() : 0)
            << ", \"selected\": " << (selected.size() > 0 ? static_cast<int>(selected[0]) : -1);
    }
    out << "}, \"modals\": [";
    std::vector<std::string> modals;
    if (const auto* tree = get_tree(); tree != nullptr && tree->get_root() != nullptr) {
        std::size_t order = 0;
        collect_modals(tree->get_root(), order, modals);
    }
    for (std::size_t i = 0; i < modals.size(); ++i) out << (i ? ", " : "") << modals[i];
    out << "]}";
    return out.str();
}
} // namespace eawr::presentation::godot_backend
