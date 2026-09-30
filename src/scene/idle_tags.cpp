#include "eawr/scene/idle_tags.hpp"

#include "scene_internal.hpp"

#include <algorithm>
#include <string_view>

namespace eawr::scene {
namespace {

[[nodiscard]] bool flag(const data::EffectiveObject& object, const std::string_view tag) {
    const data::EffectiveValue* value = object.value(tag);
    if (value == nullptr) return false;
    const std::string text = trimmed(value->value.raw_text);
    return ieq(text, "yes") || ieq(text, "true") || text == "1";
}

// Whether a comma or whitespace separated behaviour list names `behaviour`.
[[nodiscard]] bool lists(const data::EffectiveObject& object, const std::string_view tag,
                         const std::string_view behaviour) {
    const data::EffectiveValue* value = object.value(tag);
    if (value == nullptr) return false;
    const std::string_view text = value->value.raw_text;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find_first_of(", \t\r\n", start), text.size());
        if (end > start && ieq(text.substr(start, end - start), behaviour)) return true;
        start = end + 1;
    }
    return false;
}

} // namespace

IdleTags idle_tags(const data::EffectiveObject& object, const assets::MapKind kind) {
    IdleTags tags;
    tags.loop = flag(object, "Loop_Idle_Anim_00");
    if (const data::EffectiveValue* rate = object.value("Idle_Anim_00_Rate_Mod")) {
        tags.rate_mod = trimmed(rate->value.raw_text);
    }
    const bool space = kind == assets::MapKind::space;
    const std::string_view mode_list = space ? "SpaceBehavior" : "LandBehavior";
    for (const std::string_view list : {std::string_view("Behavior"), mode_list}) {
        tags.idle_behavior = tags.idle_behavior || lists(object, list, "IDLE");
        tags.dummy_starship = tags.dummy_starship || (space && lists(object, list, "DUMMY_STARSHIP"));
    }
    return tags;
}

} // namespace eawr::scene
