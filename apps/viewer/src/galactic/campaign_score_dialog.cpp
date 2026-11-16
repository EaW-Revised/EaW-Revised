#include "campaign_score_dialog.hpp"
#include "galactic_mode_internal.hpp"
#include <godot_cpp/classes/base_button.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/object.hpp>
#include <algorithm>
#include <sstream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string destination_name(platform::CampaignScoreDestination destination) {
    switch (destination) {
    case platform::CampaignScoreDestination::none: return "none";
    case platform::CampaignScoreDestination::main_menu: return "main_menu";
    case platform::CampaignScoreDestination::lan_lobby: return "lan_lobby";
    }
    return "none";
}
std::string ending_name(platform::CampaignEndingMode mode) {
    switch (mode) {
    case platform::CampaignEndingMode::solo: return "solo";
    case platform::CampaignEndingMode::lan: return "lan";
    case platform::CampaignEndingMode::other_multiplayer: return "other_multiplayer";
    }
    return "solo";
}
// Greedy word wrap of authored score text to the list's width; the authored list
// shows the whole text, so a row never clips (GHUD-S48).
std::vector<std::string> wrap_lines(const Control& list, const std::string& text) {
    std::vector<std::string> lines;
    const auto font = list.get_theme_font("font");
    const auto pixels = list.get_theme_font_size("font_size");
    const float limit = std::max(32.0F, list.get_size().x - 24.0F);
    const auto width = [&](const std::string& value) {
        return font.is_valid() ? font->get_string_size(String::utf8(value.c_str()),
            HORIZONTAL_ALIGNMENT_LEFT, -1, pixels).x : static_cast<float>(value.size() * 8);
    };
    std::string paragraph;
    const auto flush = [&] {
        std::string line, word;
        const auto push_word = [&] {
            if (word.empty()) return;
            const auto candidate = line.empty() ? word : line + ' ' + word;
            if (!line.empty() && width(candidate) > limit) { lines.push_back(line); line = word; }
            else line = candidate;
            word.clear();
        };
        for (const char character : paragraph) {
            if (character == ' ') push_word(); else word += character;
        }
        push_word();
        lines.push_back(line);
        paragraph.clear();
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') continue;
        if (text[index] == '\n') { flush(); continue; }
        if (text[index] == '\\' && index + 1 < text.size() && text[index + 1] == 'n') { flush(); ++index; continue; }
        paragraph += text[index];
    }
    flush();
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}
void caption(const BuiltGadget& gadget, const std::string& value) {
    if (!gadget.control) return;
    const String text = String::utf8(value.c_str());
    if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) label->set_text(text);
    if (auto* button = Object::cast_to<EawrUiButton>(gadget.control)) button->set_text(text);
    gadget.control->set_meta("score_caption", text);
}
}

CampaignScoreDialog::CampaignScoreDialog(Control& parent, Style style, std::function<bool()> space_draw,
    viewer::galactic::CampaignScoreExit exit)
    : style_(std::move(style)), space_draw_(std::move(space_draw)), exit_(std::move(exit)) {
    // WGP-37: the score is the topmost surface. Its own layer sits just over the campaign-end credits roll
    // (EawrCreditsScreen::layer_index), which hides the galaxy and the HUD under it (WGP-107, game-hidden).
    layer_ = memnew(CanvasLayer);
    layer_->set_name("CampaignScoreLayer");
    layer_->set_layer(EawrCreditsScreen::layer_index + 1);
    parent.add_child(layer_);
    root_ = memnew(Control);
    layer_->add_child(root_);
    root_->set_name("CampaignScoreDialog");
    root_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    root_->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    root_->hide();
}
CampaignScoreDialog::~CampaignScoreDialog() {
    close_credits();
    // The owning State tears down before its parent node.
    if (layer_) memdelete(layer_);
}
EawrCreditsScreen* CampaignScoreDialog::credits_screen() const {
    return credits_id_ && UtilityFunctions::is_instance_id_valid(credits_id_)
        ? Object::cast_to<EawrCreditsScreen>(ObjectDB::get_instance(static_cast<uint64_t>(credits_id_))) : nullptr;
}
void CampaignScoreDialog::close_credits() {
    if (auto* screen = credits_screen(); screen && !screen->finished()) screen->skip(); // frees itself
    credits_id_ = 0;
}
void CampaignScoreDialog::service_movies() {
    if (!shown_ || !presentation_) return;
    // WGP-110: the roll reaching its end is the credits-completion consumer (the rendering device is operational
    // whenever this mode draws).
    if (movie_.rolling) {
        auto* screen = credits_screen();
        if (!screen || screen->finished()) {
            credits_id_ = 0; ++credits_completed_;
            presentation_->credits_completed(movie_.token, true);
        }
    }
    viewer::galactic::advance_campaign_movies(*presentation_, movie_, [this](std::string_view name) {
        auto declared = style_.movie ? style_.movie(name) : std::nullopt;
        if (declared && declared->has_credits) {
            close_credits();
            auto* screen = style_.open_credits ? style_.open_credits() : nullptr;
            if (screen) { credits_id_ = static_cast<std::int64_t>(screen->get_instance_id()); ++credits_rolls_; }
            else declared->has_credits = false; // cannot be shown: an open failure (WGP-108)
        }
        return declared;
    });
    if (!movie_.rolling && credits_id_) close_credits();
    // WGP-107: the first continuation stops music and speech.
    if (presentation_->take_stop_music_and_speech()) {
        ++audio_stops_;
        if (style_.stop_music_and_speech) style_.stop_music_and_speech();
    }
}
bool CampaignScoreDialog::movie_key() {
    if (!shown_ || exited_ || !presentation_ || !movie_.rolling || !credits_screen()) return false;
    const auto token = movie_.token;
    // The credits roll hides the game and the main menu is not active while the score is up.
    if (!presentation_->movie_skip_key(true, true, true, false, movie_.cannot_skip)) return false;
    ++movie_keys_;
    const auto& request = presentation_->movie_request();
    if (!request || request->token != token) {
        movie_.rolling = false;
        close_credits();
        service_movies();
    }
    return true;
}
void CampaignScoreDialog::present(std::shared_ptr<const platform::CampaignEndView> view) {
    if (view == view_) return;
    view_ = std::move(view);
    close_credits();
    movie_ = {};
    presentation_.reset();
    exited_ = false; destination_ = platform::CampaignScoreDestination::none;
    shown_ = view_ && viewer::galactic::campaign_score_shown(*view_);
    if (!shown_) { root_->hide(); return; }
    // WGP-38: one presentation-stream draw per opened score screen.
    space_ = space_draw_ ? space_draw_() : true;
    drawn_ = true;
    presentation_.emplace(*view_);
    captions_ = viewer::galactic::campaign_score_captions(*view_, space_);
    root_->show();
    rebuild();
}
void CampaignScoreDialog::service_layout() {
    if (!shown_ || size_ == root_->get_size()) return;
    rebuild();
}
void CampaignScoreDialog::activate_quit(bool escape) {
    if (!shown_ || exited_ || !presentation_) return;
    if (escape) ++escapes_;
    // The dialog is the only modal surface while it is shown, so it is topmost.
    const auto destination = presentation_->score_exit(true, true);
    if (destination == platform::CampaignScoreDestination::none) return;
    exited_ = true; destination_ = destination; ++exits_;
    movie_.rolling = false; close_credits(); // the exit clears the retained movie names
    if (exit_) exit_(destination);
}
void CampaignScoreDialog::rebuild() {
    if (dialog_ && dialog_->frame) { dialog_->frame->hide(); dialog_->frame->queue_free(); }
    // A rebuild can run inside a relay's callback; defer its deletion.
    for (auto* relay : relays_) relay->queue_free();
    relays_.clear(); dialog_.reset();
    size_ = root_->get_size();
    // An unlaid-out root has no reference space yet; service_layout retries on its first size.
    if (!shown_ || !style_.dialog || size_.x < 1 || size_.y < 1) return;
    auto built = style_.dialog("IDD_CAMPAIGN_SCORE_DIALOG", size_);
    if (!built.frame) return;
    dialog_ = std::move(built);
    root_->add_child(dialog_->frame);
    ++rebuilds_;
    for (auto& gadget : dialog_->gadgets) {
        if (gadget.class_name == "PETROGLYPH_DIALOG_IMAGE" && gadget.id == "IDC_BATTLE_VICTORY_IMAGE") {
            auto* image = memnew(TextureRect);
            image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            image->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            image->set_position(Vector2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y))
                - dialog_->frame->get_position());
            image->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
            dialog_->frame->add_child(image); gadget.control = image;
        }
        if (!gadget.control) continue;
        if (gadget.id == "IDC_QUIT_BUTTON") {
            auto* button = Object::cast_to<BaseButton>(gadget.control);
            if (!button) continue;
            auto* relay = memnew(EawrOverlayButton); root_->add_child(relay); relay->hide();
            relay->set_action([this] { activate_quit(); }); relays_.push_back(relay);
            button->connect("pressed", callable_mp(relay, &EawrOverlayButton::activate));
        }
    }
    refresh();
}
void CampaignScoreDialog::refresh() {
    if (!dialog_) return;
    for (auto& gadget : dialog_->gadgets) {
        auto* control = gadget.control;
        if (!control) continue;
        const auto& id = gadget.id;
        // WGP-37: the score opens with its close button hidden.
        if (id == "IDC_CLOSE_BUTTON") { control->hide(); continue; }
        if (id == "IDC_VICTORY_STATIC") caption(gadget, style_.text(captions_.heading_key));
        else if (id == "IDC_TITLE_STATIC") caption(gadget, captions_.title_key.empty() ? std::string{} : style_.text(captions_.title_key));
        else if (id == "IDC_MILITARY_EFFICIENCY_STATIC") caption(gadget, captions_.efficiencies[0]);
        else if (id == "IDC_CONQUEST_EFFICIENCY_STATIC") caption(gadget, captions_.efficiencies[1]);
        else if (id == "IDC_KILL_EFFICIENCY_STATIC") caption(gadget, captions_.efficiencies[2]);
        else if (id == "IDC_QUIT_BUTTON") caption(gadget, style_.text(captions_.button_key));
        else if (id == "IDC_VICTORY_LIST") {
            if (auto* list = Object::cast_to<ItemList>(control)) {
                list->clear();
                const auto text = captions_.list_key.empty() ? std::string{} : style_.text(captions_.list_key);
                for (const auto& line : wrap_lines(*list, text)) {
                    const auto index = list->add_item(String::utf8(line.c_str()));
                    list->set_item_selectable(index, false);
                }
            }
        } else if (id == "IDC_BATTLE_VICTORY_IMAGE") {
            if (auto* image = Object::cast_to<TextureRect>(control)) {
                const auto texture = captions_.picture.empty() || !style_.texture
                    ? Ref<Texture2D>{} : style_.texture(captions_.picture);
                image->set_texture(texture);
                image->set_visible(texture.is_valid());
                image->set_meta("score_caption", String::utf8(captions_.picture.c_str()));
            }
        }
    }
}
std::string CampaignScoreDialog::report_json() const {
    using galactic_detail::json;
    std::ostringstream out;
    const bool have = view_ != nullptr;
    out << "{\"shown\":" << (shown_ ? "true" : "false") << ",\"mounted\":" << (dialog_ ? "true" : "false")
        << ",\"rebuilds\":" << rebuilds_ << ",\"exits\":" << exits_ << ",\"escapes\":" << escapes_
        << ",\"destination\":" << json(destination_name(destination_))
        << ",\"route\":" << json(have ? (view_->route == platform::CampaignEndRoute::score ? "score" : "multiplayer_quit") : "")
        << ",\"ending_mode\":" << json(have ? ending_name(view_->ending_mode) : "")
        << ",\"local_won\":" << (have && view_->local_won ? "true" : "false")
        << ",\"explicit_quit\":" << (have && view_->explicit_quit ? "true" : "false")
        << ",\"winner_faction\":" << json(have ? view_->winner_faction : "")
        << ",\"inputs_bound\":" << (have && view_->inputs ? "true" : "false")
        << ",\"provider_seam\":" << json(have ? view_->provider_seam : "")
        << ",\"space_draw\":" << (drawn_ ? (space_ ? "true" : "false") : "null")
        << ",\"heading_key\":" << json(captions_.heading_key) << ",\"list_key\":" << json(captions_.list_key)
        << ",\"picture\":" << json(captions_.picture) << ",\"button_key\":" << json(captions_.button_key)
        << ",\"title_key\":" << json(captions_.title_key)
        << ",\"efficiencies\":[" << json(captions_.efficiencies[0]) << ',' << json(captions_.efficiencies[1])
        << ',' << json(captions_.efficiencies[2]) << ']'
        << ",\"movie_request\":" << json(presentation_ && presentation_->movie_request() ? presentation_->movie_request()->name : "")
        << ",\"movie\":{\"name\":" << json(movie_.name) << ",\"rolling\":" << (movie_.rolling ? "true" : "false")
        << ",\"cannot_skip\":" << (movie_.cannot_skip ? "true" : "false")
        << ",\"credits_open\":" << (credits_screen() ? "true" : "false")
        << ",\"credits_layer\":" << (credits_screen() ? credits_screen()->get_layer() : 0)
        << ",\"score_layer\":" << (layer_ ? layer_->get_layer() : 0)
        << ",\"credits_rolls\":" << credits_rolls_ << ",\"credits_completed\":" << credits_completed_
        << ",\"keys\":" << movie_keys_ << ",\"audio_stops\":" << audio_stops_ << '}'
        << ",\"movie_seam\":\"faction win and finale movies need the full-screen player of the story campaigns (#1951)\""
        << ",\"scoring_seam\":\"GHUD-S61 scoring ledger and script title/efficiency provider\""
        << ",\"controls\":[";
    std::size_t count{};
    if (dialog_) for (const auto& gadget : dialog_->gadgets) if (gadget.control) {
        const auto rect = gadget.control->get_global_rect();
        const auto* button = Object::cast_to<BaseButton>(gadget.control);
        const String text = gadget.control->get_meta("score_caption", String());
        out << (count++ ? "," : "") << "{\"name\":" << json(gadget.id)
            << ",\"text\":" << json(std::string(text.utf8().get_data()))
            << ",\"visible\":" << (gadget.control->is_visible_in_tree() ? "true" : "false")
            << ",\"disabled\":" << (button && button->is_disabled() ? "true" : "false")
            << ",\"rect\":[" << rect.position.x << ',' << rect.position.y << ',' << rect.size.x << ',' << rect.size.y << "]";
        if (const auto* list = Object::cast_to<ItemList>(gadget.control)) {
            out << ",\"items\":[";
            for (int item = 0; item < list->get_item_count(); ++item)
                out << (item ? "," : "") << json(std::string(list->get_item_text(item).utf8().get_data()));
            out << ']';
        }
        out << '}';
    }
    out << "]}";
    return out.str();
}
}
