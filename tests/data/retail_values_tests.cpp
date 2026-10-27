#include "data_test_support.hpp"
#include "eawr/data/retail_values.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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

void integer_constant_contracts() {
    // #4992: the C atoi read of an integer game constant: no sign or range refusal.
    const std::pair<std::string_view, std::int32_t> cases[] = {
        {"6", 6}, {" -1 ", -1}, {"+7", 7}, {"0", 0}, {"2.5", 2}, {"-0.5", 0}, {"65537", 65537}, {"12abc", 12},
        {"abc", 0}, {"", 0}, {"-", 0}, {"1e3", 1}, {"0.8f", 0}, {"\t\n 42", 42}, {"2147483647", 2147483647},
        {"2147483648", 2147483647}, {"99999999999999999999", 2147483647}, {"-2147483648", INT32_MIN},
        {"-99999999999999999999", INT32_MIN},
    };
    for (const auto& [text, value] : cases) {
        expect(retail::integer_constant(text) == value, std::string("integer constant (atoi): '") + std::string(text) + "'");
    }
    expect(static_cast<std::uint32_t>(retail::integer_constant("-1")) == 4294967295U,
        "integer constant: an unsigned consumer sees -1 as 4294967295");
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

// LSL-P1: the retail float parse never fails; boundary values -1, 0, max, fractional, empty and junk.
void float_contracts() {
    using eawr::sim::math::Fixed;
    const auto expect_float = [](const std::string_view text, const Fixed value, const bool exact) {
        const auto parsed = retail::float_value(text);
        expect(parsed && parsed->value == value && parsed->exact == exact, std::string("LSL-P1 float: '") + std::string(text) + "'");
    };
    const auto d = [](const std::string_view text) { return Fixed::from_decimal(text).value(); };
    const auto top = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    expect_float("-1", d("-1"), true);
    expect_float("0", Fixed{}, true);
    expect_float("-0.4", d("-0.4"), true);
    expect_float(".5", d("0.5"), true);
    expect_float("9999999", d("9999999"), true);
    expect_float("1.5f", d("1.5"), false);
    expect_float("1,5", d("1"), false);
    expect_float("abc", Fixed{}, false);
    expect_float("-", Fixed{}, false);
    expect_float("1e30", top, false);
    expect_float("-1e30", Fixed::from_raw(-std::numeric_limits<std::int64_t>::max()), false);
    expect_float("inf", top, false);
    expect_float("1e-30", Fixed{}, true);
    expect(!retail::float_value("") && !retail::float_value("  ") && !retail::float_value("TBD"),
        "LSL-P1: a placeholder keeps the earlier or default value");
}

// LSL-P2: the retail signed integer parse truncates, saturates at 32 bits and never fails.
void integer_contracts() {
    const auto expect_integer = [](const std::string_view text, const std::int32_t value, const bool exact) {
        const auto parsed = retail::integer_value(text);
        expect(parsed && parsed->value == value && parsed->exact == exact, std::string("LSL-P2 integer: '") + std::string(text) + "'");
    };
    expect_integer("-1", -1, true);
    expect_integer("0", 0, true);
    expect_integer(" +9 ", 9, true);
    expect_integer("2.9", 2, false);
    expect_integer("-2.7", -2, false);
    expect_integer("-0.5", 0, false);
    expect_integer("1e3", 1, false);
    expect_integer("abc", 0, false);
    expect_integer("2147483647", 2147483647, true);
    expect_integer("-2147483648", std::numeric_limits<std::int32_t>::min(), true);
    expect_integer("3000000000", 2147483647, false);
    expect_integer("-3000000000", std::numeric_limits<std::int32_t>::min(), false);
    expect_integer("99999999999999999999", 2147483647, false);
    expect(!retail::integer_value("") && !retail::integer_value("TBD"), "LSL-P2: a placeholder keeps the earlier or default value");
}

void retail_value_contracts() {
    placeholder_contracts();
    boolean_contracts();
    number_contracts();
    integer_constant_contracts();
    float_contracts();
    integer_contracts();
    list_contracts();
}

} // namespace eawr::tests::data_contracts
