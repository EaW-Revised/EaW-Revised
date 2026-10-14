#include "data_test_support.hpp"

#include "eawr/data/retail_values.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::tests::data_contracts {
namespace {

namespace retail = eawr::data::retail;
using eawr::data::Definition;
using eawr::data::XmlNode;

[[nodiscard]] Definition layer(const std::string& id, const std::vector<std::pair<std::string, std::string>>& tags) {
    Definition result;
    result.id = id;
    result.root.name = "Squadron";
    for (const auto& [name, text] : tags) {
        XmlNode node;
        node.name = name;
        node.raw_text = text;
        result.root.children.push_back(std::move(node));
    }
    return result;
}

[[nodiscard]] std::vector<std::string> texts(const std::vector<const XmlNode*>& nodes) {
    std::vector<std::string> result;
    for (const auto* node : nodes) result.push_back(node->raw_text);
    return result;
}

void placeholder_contracts() {
    expect(retail::is_placeholder("") && retail::is_placeholder(" \t\r\n") && retail::is_placeholder("TBD")
        && retail::is_placeholder("  TBD "), "AU-81: empty and TBD are placeholders");
    expect(!retail::is_placeholder("tbd") && !retail::is_placeholder("TBDX") && !retail::is_placeholder("0"),
        "AU-81: TBD is an exact, case-sensitive match");
}

void boolean_contracts() {
    for (const auto* text : {"1", "yes", "Yes", "YES", "y", "true", "True", "T", "trUE", " Yikes ", "Ture"}) {
        expect(retail::boolean(text) == std::optional<bool>{true}, std::string("AU-82 true: ") + text);
    }
    for (const auto* text : {"0", "no", "No", "false", "FALSE", "nope", "2", "on", "-1", "x"}) {
        expect(retail::boolean(text) == std::optional<bool>{false}, std::string("AU-82 false: ") + text);
    }
    expect(!retail::boolean("") && !retail::boolean("TBD"), "AU-82: a placeholder is no boolean value");
}

void number_contracts() {
    using eawr::sim::math::Fixed;
    const auto expect_number = [](const std::string_view text, const std::string_view decimal, const bool trailing) {
        const auto parsed = retail::leading_number(text);
        expect(parsed && parsed->value.raw() == Fixed::from_decimal(decimal).value().raw()
            && parsed->trailing_text == trailing, std::string("AU-82 leading number: ") + std::string(text));
    };
    expect_number("12", "12", false);
    expect_number("  -3.5  ", "-3.5", false);
    expect_number("0.8f", "0.8", true);
    expect_number("+2.", "2", false);
    expect_number(".25", "0.25", false);
    expect_number("1e2", "100", false);
    expect_number("1e", "1", true);
    expect_number("7 seconds", "7", true);
    expect_number("4,5", "4", true);
    for (const auto* text : {"", "TBD", "abc", "-", ".", "e5", "f1"}) {
        expect(!retail::leading_number(text), std::string("AU-82: no leading number in '") + text + "'");
    }
}

void list_contracts() {
    expect(retail::list_rule("squadron_units") == retail::ListRule::replace_by_variant_all
        && retail::list_rule("Starting_Spawned_Units_Tech_0") == retail::ListRule::append
        && retail::list_rule("Reserve_Spawned_Units_Tech_0") == retail::ListRule::append
        && retail::list_rule("SQUADRON_OFFSETS") == retail::ListRule::replace_in_variant
        && retail::list_rule("Fire_Inaccuracy_Distance") == retail::ListRule::replace_in_variant
        && !retail::list_rule("Company_Units"), "AU-82 / ML-26: the audited list tags and their rules");

    const auto base = layer("Base", {{"List", "a"}, {"List", ""}, {"List", "b"}, {"Single", "base"}});
    const auto middle = layer("Middle", {{"List", "c"}, {"List", "TBD"}, {"List", "d"}, {"Single", "middle"}, {"Single", "TBD"}});
    const auto top = layer("Top", {{"Single", ""}, {"Other", "x"}});
    const std::array<const Definition*, 3> chain{&top, &middle, &base};
    const std::array<const Definition*, 1> alone{&base};

    expect(texts(retail::list(chain, "list", retail::ListRule::append)) == std::vector<std::string>{"a", "b", "c", "d"},
        "AU-82 append: base first, variants appended, placeholders skipped");
    expect(texts(retail::list(chain, "List", retail::ListRule::replace_in_variant)) == std::vector<std::string>{"d"},
        "AU-82 replace: a variant's last occurrence replaces the inherited list");
    expect(texts(retail::list(alone, "List", retail::ListRule::replace_in_variant)) == std::vector<std::string>{"a", "b"},
        "AU-82 replace: a base type appends every occurrence");
    const auto silent = layer("Silent", {{"List", "TBD"}});
    const std::array<const Definition*, 2> placeholder_variant{&silent, &base};
    expect(texts(retail::list(placeholder_variant, "List", retail::ListRule::replace_in_variant))
            == std::vector<std::string>{"a", "b"},
        "AU-81/82: a variant's placeholder occurrence keeps the inherited list");

    const auto rule = retail::ListRule::replace_by_variant_all;
    const std::array<const Definition*, 2> omitted_variant{&top, &base};
    expect(texts(retail::list(omitted_variant, "List", rule)) == std::vector<std::string>{"a", "b"},
        "ML-26: omission inherits every base occurrence");
    expect(texts(retail::list(placeholder_variant, "List", rule)) == std::vector<std::string>{"a", "b"},
        "ML-26: a placeholder-only variant preserves inherited members");
    const auto one = layer("One", {{"List", ""}, {"List", "TBD"}, {"List", "x, y"}});
    const std::array<const Definition*, 2> one_variant{&one, &base};
    expect(texts(retail::list(one_variant, "List", rule)) == std::vector<std::string>{"x, y"},
        "ML-26: the first effective occurrence replaces inherited members after placeholders");
    expect(texts(retail::list(chain, "List", rule)) == std::vector<std::string>{"c", "d"},
        "ML-26: retain all variant occurrences in order through an omitted outer layer");
    const std::array<const Definition*, 3> replaced_again{&one, &middle, &base};
    expect(texts(retail::list(replaced_again, "List", rule)) == std::vector<std::string>{"x, y"},
        "ML-26: a further authored variant replaces the complete inherited roster");
    expect(texts(retail::list(alone, "List", rule)) == std::vector<std::string>{"a", "b"},
        "ML-26: base occurrences append without deduplication");

    const auto* single = retail::single(chain, "single");
    expect(single != nullptr && single->raw_text == "middle",
        "AU-80/81/83: the last real value wins; later placeholders change nothing");
    expect(retail::single(chain, "Missing") == nullptr, "an absent tag has no value");
    const auto blank = layer("Blank", {{"Single", " "}});
    const std::array<const Definition*, 1> only_blank{&blank};
    expect(retail::single(only_blank, "Single") == nullptr, "AU-81: only placeholders means no value");

    auto nested = layer("Nested", {{"Single", ""}});
    XmlNode member;
    member.name = "Type";
    member.raw_text = "DEFEND";
    nested.root.children.front().children.push_back(std::move(member));
    const std::array<const Definition*, 2> nested_chain{&nested, &base};
    expect(retail::single(nested_chain, "Single") == &nested.root.children.front(),
        "AU-81: nested XML records with empty scalar text remain authored values");
}

} // namespace

void retail_value_contracts() {
    placeholder_contracts();
    boolean_contracts();
    number_contracts();
    list_contracts();
}

} // namespace eawr::tests::data_contracts
