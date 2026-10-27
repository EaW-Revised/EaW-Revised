#include "eawr/mods/shader_translation.hpp"
#include "eawr/mods/chain.hpp"

namespace eawr::mods {
namespace {
const steam::JsonValue* unique_member(const steam::JsonValue& object, std::string_view key, bool& malformed) {
    if (object.kind != steam::JsonValue::Kind::object || object.keys.size() != object.values.size()) {
        malformed = true;
        return nullptr;
    }
    const steam::JsonValue* found = nullptr;
    for (std::size_t i = 0; i < object.keys.size(); ++i) {
        if (object.keys[i] == key) {
            if (found) { malformed = true; return nullptr; }
            found = &object.values[i];
        }
    }
    return found;
}
} // namespace
ShaderTranslationDecision shader_translation_policy(const ModRecord* contributor) {
    if (!contributor) return {};
    ShaderTranslationDecision result{ShaderTranslationPolicy::allowed, contributor->key};
    // A rejected modinfo may have contained an opt-out which the metadata
    // reader could not preserve. Fail closed for this contributor (STO-04).
    if (contributor->has_warning(codes::malformed_modinfo))
        return {ShaderTranslationPolicy::malformed, contributor->key, "shader_translation"};
    // Discovery records with no metadata have no policy.
    if (contributor->metadata.kind == steam::JsonValue::Kind::null) return result;
    bool malformed = false;
    const auto* modinfo = unique_member(contributor->metadata, "modinfo", malformed);
    const auto* flag = modinfo ? unique_member(*modinfo, "shader_translation", malformed) : nullptr;
    if (flag && flag->kind != steam::JsonValue::Kind::boolean) malformed = true;
    if (malformed) {
        result.policy = ShaderTranslationPolicy::malformed;
        result.diagnostic_field = "shader_translation";
    }
    else if (flag && !flag->boolean) result.policy = ShaderTranslationPolicy::opted_out;
    return result;
}
} // namespace eawr::mods
