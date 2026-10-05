#include "data_test_support.hpp"

#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::tests::data_contracts {
namespace {
void byte_provenance_contracts() {
    const auto folder = std::filesystem::temp_directory_path() /
        ("eawr-xml-byte-provenance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path folder;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(folder, ignored); }
    } cleanup{folder};
    std::filesystem::create_directories(folder / "XML");
    // LF advances the line; CR and each UTF-8 byte still advance the byte column.
    const std::string bytes = "\xEF\xBB\xBF" "<?xml version=\"1.0\"?>\r\n"
        "<!-- snow: \xE9\x9B\xAA -->\r<Root\n first=\"A\"\r\n second='B'><Child\n"
        " flag=\"yes\">value</Child><Tail/></Root>\n";
    {
        std::ofstream stream(folder / "XML/provenance.xml", std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const std::array mounts{eawr::vfs::MountSpec{"synthetic", folder, "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "byte-provenance fixture mounts");
    if (!filesystem) return;
    const auto document = eawr::data::load_document(filesystem.value(), "data/xml/provenance.xml");
    expect(static_cast<bool>(document), "mixed byte/line-ending document loads");
    if (!document) return;
    const auto& root = document.value().root;
    expect(root.source.line == 2 && root.source.column == 21,
           "root column counts UTF-8 bytes and lone CR after the preceding LF");
    expect(root.attributes.size() == 2 && root.attributes[0].source.line == 3 &&
               root.attributes[0].source.column == 2 && root.attributes[1].source.line == 4 &&
               root.attributes[1].source.column == 2,
           "multiline attributes retain byte columns across LF and CRLF");
    expect(root.children.size() == 2 && root.children[0].source.line == 4 &&
               root.children[0].source.column == 14 && root.children[1].source.line == 5 &&
               root.children[1].source.column == 27,
           "nested and same-line sibling source positions remain authored offsets");
    if (!root.children.empty()) {
        const auto& child = root.children[0];
        expect(child.attributes.size() == 1 && child.attributes[0].source.line == 5 &&
                   child.attributes[0].source.column == 2 && child.raw_text == "value",
               "nested attribute locations and original text are retained");
    }
}
} // namespace

void schema_contracts(const eawr::data::Catalog& catalog) {
    byte_provenance_contracts();
    const auto* base = catalog.find("BASE");
    const eawr::data::XmlNode* number = nullptr;
    if (base) {
        expect(base->root.attributes.size() == 3 && base->root.attributes[0].name == "Name" &&
                   base->root.attributes[1].name == "zAttr" && base->root.attributes[2].name == "aAttr" &&
                   base->root.attributes[1].value == "Z" && base->root.attributes[0].source.line == 2 &&
                   base->root.attributes[1].source.line == 3 && base->root.attributes[2].source.line == 4 &&
                   base->root.source.layer_id == "base" &&
                   base->root.source.logical_path == "data/xml/objects-a.xml" && base->root.source.line > 0,
               "raw attribute order/spelling and VFS path/layer/line provenance are retained");
        const auto found = std::find_if(base->root.children.begin(), base->root.children.end(),
            [](const auto& child) { return child.name == "Numeric"; });
        if (found != base->root.children.end()) number = &*found;

        const auto health = std::find_if(base->root.children.begin(), base->root.children.end(),
            [](const auto& child) { return child.name == "Tactical_Health"; });
        const auto nested = std::find_if(base->root.children.begin(), base->root.children.end(),
            [](const auto& child) { return child.name == "Nested"; });
        bool schema_path_ok = health != base->root.children.end() &&
                              health->schema_status == eawr::data::SchemaStatus::known &&
                              health->schema_ref == "eaw/tags/GameObjectType.yaml#L501";
        if (nested != base->root.children.end()) {
            const auto nested_health = std::find_if(nested->children.begin(), nested->children.end(),
                [](const auto& child) { return child.name == "Tactical_Health"; });
            schema_path_ok = schema_path_ok && nested_health != nested->children.end() &&
                             nested_health->schema_status == eawr::data::SchemaStatus::unknown;
        } else {
            schema_path_ok = false;
        }
        expect(schema_path_ok,
               "schema classification and reference retain the accepted full tag-path key");
    }
    const auto* faction = catalog.find("FACTION_A");
    bool deprecated_schema_ok = false;
    if (faction != nullptr) {
        const auto deprecated = std::find_if(faction->root.children.begin(), faction->root.children.end(),
            [](const auto& child) { return child.name == "Standalone_Space_Maps_Special_Weapon_B"; });
        deprecated_schema_ok = deprecated != faction->root.children.end() &&
                               deprecated->schema_status == eawr::data::SchemaStatus::deprecated &&
                               deprecated->schema_ref == "eaw/tags/Faction.yaml#L481";
    }
    expect(deprecated_schema_ok,
           "packed schema retains deprecated classification and reference");
    expect(number != nullptr, "numeric source node remains available");
    if (number) {
        auto fixed = eawr::data::fixed_value(*number);
        expect(number->raw_text == " 1.5 " && fixed &&
                   fixed.value().raw() == eawr::sim::math::Fixed::scale * 3 / 2,
           "numeric lexeme converts only through accepted Fixed API");
    }
    if (base) {
        const auto overflow_node = std::find_if(base->root.children.begin(), base->root.children.end(),
            [](const auto& child) { return child.name == "NumericOverflow"; });
        expect(overflow_node != base->root.children.end() && !eawr::data::fixed_value(*overflow_node),
               "fixed conversion propagates overflow instead of inventing a gameplay default");
    }
}
} // namespace eawr::tests::data_contracts
