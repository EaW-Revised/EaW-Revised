#include "data_test_support.hpp"

#include "eawr/data/xml.hpp"

#include <string>
#include <thread>
#include <vector>

namespace eawr::tests::data_contracts {

namespace {

[[nodiscard]] std::string text(const eawr::data::Catalog& catalog, const std::string& id, const std::string& tag) {
    auto resolved = catalog.resolve(id);
    if (!resolved) return "<unresolved>";
    const auto* value = resolved.value().value(tag);
    return value == nullptr ? "<absent>" : value->value.raw_text;
}

} // namespace

// with_overrides (docs/tag-applied-check.md): the perturbation check's in-memory data change.
void override_contracts(const eawr::data::Catalog& catalog, const eawr::vfs::Vfs& filesystem) {
    using eawr::data::ValueOverride;
    const std::vector<ValueOverride> changes{
        {"TOP", "ReplaceTag", {"changed"}},     // authored by the object itself
        {"TOP", "Tactical_Health", {"250"}},    // inherited from BASE
        {"MID", "MergeTag", {"X"}},             // a merge tag: the override is the value
        {"TOP", "Nested/Leaf/Deep", {"deeper"}},
        {"BASE", "BaseOnly", {}},               // removed
    };
    auto made = eawr::data::with_overrides(catalog, changes);
    expect(static_cast<bool>(made), "with_overrides applies authored, inherited, merge, nested and removed values");
    if (!made) return;
    const auto& changed = made.value();
    expect(text(changed, "TOP", "ReplaceTag") == "changed", "an authored value is replaced");
    expect(text(changed, "TOP", "Tactical_Health") == "250", "an inherited value is overridden by the object's own");
    expect(text(changed, "BASE", "Tactical_Health") == "100", "the base keeps its inherited value");
    expect(text(changed, "MID", "MergeTag") == "X", "a merge tag's override is not merged with the base");
    expect(text(changed, "TOP", "MergeTag") == "X, C", "a variant of the changed object merges onto the new value");
    expect(text(changed, "BASE", "BaseOnly") == "<absent>", "an empty value list removes the tag");
    expect(text(changed, "TOP", "BaseOnly") == "<absent>", "variants of a changed base see the change");
    if (auto top = changed.resolve("TOP")) {
        const auto* nested = top.value().value("Nested");
        const bool deep = nested != nullptr && !nested->value.children.empty()
            && !nested->value.children.front().children.empty()
            && nested->value.children.front().children.front().raw_text == "deeper";
        expect(deep, "a nested path changes the leaf of the object's own element");
    }
    expect(changed.generation() != catalog.generation(), "an overridden catalog has its own generation");
    expect(changed.find("TOP") != catalog.find("TOP"), "find sees the changed definition");

    // The load it shares is untouched, cached or not.
    expect(text(catalog, "TOP", "ReplaceTag") == "top", "the original catalog keeps its values");
    expect(text(catalog, "BASE", "BaseOnly") == "kept", "the original catalog keeps a removed tag");
    expect(text(catalog, "TOP", "Tactical_Health") == "100", "the original catalog keeps the inherited value");

    // Overrides stack, and a later override of the same tag wins.
    const std::vector<ValueOverride> again{{"TOP", "ReplaceTag", {"again"}}};
    auto stacked = eawr::data::with_overrides(changed, again);
    expect(stacked && text(stacked.value(), "TOP", "ReplaceTag") == "again"
               && text(stacked.value(), "TOP", "Tactical_Health") == "250",
           "an override of an overridden catalog keeps the earlier changes");

    const std::vector<ValueOverride> unknown{{"NO_SUCH_OBJECT", "ReplaceTag", {"x"}}};
    expect(!eawr::data::with_overrides(catalog, unknown), "an unknown object is an error");
    const std::vector<ValueOverride> no_container{{"BASE", "Absent/Leaf", {"x"}}};
    expect(!eawr::data::with_overrides(catalog, no_container), "a path below an absent element is an error");

    // A document's value, for the loads of this thread while the scope lives.
    const auto marker = [&filesystem]() -> std::string {
        auto document = eawr::data::load_document(filesystem, "data/xml/objects-a.xml");
        if (!document || document.value().root.children.empty()) return "<unloaded>";
        for (const auto& child : document.value().root.children.back().children) {
            if (child.name == "Marker_Test") return child.raw_text;
        }
        return "<absent>";
    };
    {
        const eawr::data::DocumentOverrides scope({{"Data/XML/Objects-A.xml", "SpaceUnit/Marker_Test", {"v"}}});
        expect(marker() == "v", "a document override changes load_document's tree on its thread");
        std::string elsewhere;
        std::thread other([&] { elsewhere = marker(); });
        other.join();
        expect(elsewhere == "<absent>", "a document override is not seen by another thread");
    }
    expect(marker() == "<absent>", "a document override ends with its scope");
}

} // namespace eawr::tests::data_contracts
