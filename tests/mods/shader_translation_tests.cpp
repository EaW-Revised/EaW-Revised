#include "eawr/mods/chain.hpp"
#include "eawr/mods/shader_translation.hpp"
#include <iostream>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
using namespace eawr::mods;
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error("shader policy line " + std::to_string(__LINE__) + ": " #value); } while (false)
ModRecord record(std::string_view text, std::string key = "mods:synthetic") {
    const auto info = parse_modinfo(std::string("{\"name\":\"Synthetic\",") + std::string(text.substr(1)));
    REQUIRE(info);
    ModRecord result;
    result.key = std::move(key);
    result.metadata.kind = steam::JsonValue::Kind::object;
    result.metadata.keys = {"modinfo"};
    result.metadata.values = {info->metadata};
    return result;
}
void schema() {
    REQUIRE(shader_translation_policy(nullptr).translation_allowed());
    for (const auto text : {R"({})", R"({"shader_translation":true})", R"({"Shader_Translation":false})"}) {
        auto value = record(text);
        REQUIRE(shader_translation_policy(&value).policy == ShaderTranslationPolicy::allowed);
    }
    auto value = record(R"({"shader_translation":false})");
    const auto decision = shader_translation_policy(&value);
    REQUIRE(!decision.translation_allowed());
    REQUIRE(decision.contributor_key == "mods:synthetic");
    REQUIRE(value.metadata.values[0].member("shader_translation"));
    for (const auto text : {R"({"shader_translation":null})", R"({"shader_translation":0})",
         R"({"shader_translation":"false"})", R"({"shader_translation":[]})", R"({"shader_translation":{}})",
         R"({"shader_translation":false,"shader_translation":true})"}) {
        auto bad = record(text);
        const auto malformed = shader_translation_policy(&bad);
        REQUIRE(malformed.policy == ShaderTranslationPolicy::malformed);
        REQUIRE(!malformed.translation_allowed());
        REQUIRE(malformed.diagnostic_field == "shader_translation");
    }
    value.metadata.values[0].values.clear();
    REQUIRE(shader_translation_policy(&value).policy == ShaderTranslationPolicy::malformed);
    value.metadata = {};
    REQUIRE(shader_translation_policy(&value).policy == ShaderTranslationPolicy::allowed);
    value.metadata.kind = steam::JsonValue::Kind::object;
    REQUIRE(shader_translation_policy(&value).policy == ShaderTranslationPolicy::allowed);
    value.metadata.keys = {"modinfo"};
    value.metadata.values = {steam::JsonValue{}};
    REQUIRE(shader_translation_policy(&value).policy == ShaderTranslationPolicy::malformed);
    value = record(R"({"shader_translation":false})");
    value.metadata.keys.push_back("modinfo");
    value.metadata.values.push_back(value.metadata.values.front());
    REQUIRE(shader_translation_policy(&value).policy == ShaderTranslationPolicy::malformed);
}
void rejected_modinfo() {
    namespace fs = std::filesystem;
    struct Fixture {
        fs::path root = fs::temp_directory_path() /
            ("eawr-shader-policy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Fixture() { fs::create_directory(root); }
        ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    } fixture;
    {
        std::ofstream file(fixture.root / "modinfo.json");
        file << R"({"name":"Synthetic","version":1,"shader_translation":false})";
        REQUIRE(file.good());
    }
    ModRecord contributor;
    contributor.key = "mods:rejected";
    contributor.root = steam::path_to_utf8(fixture.root);
    contributor.has_data = true;
    Catalog catalog;
    catalog.records.push_back(contributor);
    read_mod_metadata(catalog);
    REQUIRE(catalog.records.size() == 1);
    const auto& selected = catalog.records.front();
    REQUIRE(selected.has_warning(codes::malformed_modinfo));
    REQUIRE(!selected.metadata.member("modinfo"));
    const auto decision = shader_translation_policy(&selected);
    REQUIRE(decision.policy == ShaderTranslationPolicy::malformed);
    REQUIRE(!decision.translation_allowed());
    REQUIRE(decision.diagnostic_field == "shader_translation");
    REQUIRE(decision.contributor_key == contributor.key);
    REQUIRE(shader_translation_policy(nullptr).translation_allowed());
}
void contributor() {
    auto parent = record(R"({"shader_translation":false})", "mods:parent");
    auto child = record(R"({"shader_translation":true})", "mods:child");
    // The same selected contributor is passed for either FX or FXO; the leaf is irrelevant.
    REQUIRE(!shader_translation_policy(&parent).translation_allowed());
    REQUIRE(shader_translation_policy(&child).translation_allowed());
    REQUIRE(shader_translation_policy(nullptr).contributor_key.empty());
    child.metadata.values[0].values.back().boolean = false;
    REQUIRE(!shader_translation_policy(&child).translation_allowed());
}
} // namespace
int main() {
    try { schema(); contributor(); rejected_modinfo(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
