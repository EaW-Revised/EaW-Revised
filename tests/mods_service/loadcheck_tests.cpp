#include "report.hpp"
#include "eawr/core/sha256.hpp"
#include <iostream>
int main() {
    eawr::loadcheck::Groups groups;
    using eawr::core::Severity;
    for (int i = 0; i < 5; ++i) eawr::loadcheck::add(groups, "B", std::to_string(i), Severity::warning);
    eawr::loadcheck::add(groups, "A", "quoted\"", Severity::info);
    if (groups.at("B").count != 5 || groups.at("B").examples != std::vector<std::string>{"0", "1", "2"}) return 1;
    const auto text = eawr::loadcheck::groups_json(groups);
    if (text.find("\"A\"") >= text.find("\"B\"") || text.find("quoted\\\"") == std::string::npos) return 2;
    if (text.find("\"severity\":\"warning\"") == std::string::npos || eawr::loadcheck::count(groups, Severity::warning) != 5
        || eawr::loadcheck::count(groups, Severity::error) != 0 || eawr::loadcheck::count(groups, Severity::info) != 1) return 7;
    eawr::loadcheck::add(groups, "B", "failure", Severity::error);
    eawr::loadcheck::add(groups, "B", "later warning", Severity::warning);
    if (groups.at("B").severity != Severity::error || eawr::loadcheck::count(groups, Severity::error) != 7
        || eawr::loadcheck::count(groups, Severity::warning) != 0) return 8;
    {
        // A malformed XML diagnostic names its logical file, line and column in the report example.
        eawr::loadcheck::Groups xml;
        eawr::core::Diagnostic malformed;
        malformed.code = "EAWR-XML-0001";
        malformed.severity = Severity::error;
        malformed.message = "malformed XML: Start-end tags mismatch";
        malformed.logical_path = "data/xml/broken_unit.xml";
        malformed.line = 12;
        malformed.column = 5;
        eawr::loadcheck::add(xml, malformed);
        if (xml.at("EAWR-XML-0001").examples.at(0) !=
            "malformed XML: Start-end tags mismatch (file data/xml/broken_unit.xml:12:5)") return 9;
        malformed.logical_path.reset();
        eawr::loadcheck::Groups bare;
        eawr::loadcheck::add(bare, malformed);
        if (bare.at("EAWR-XML-0001").examples.at(0) != "malformed XML: Start-end tags mismatch") return 10;
    }
    eawr::sim::MatchIdentity a;
    a.domains = {eawr::sim::identity_domain("rules", "one"), eawr::sim::identity_domain("map-path", "map-a"),
        eawr::sim::identity_domain("map-digest", "aaa"), eawr::sim::identity_domain("units", "units")};
    auto b = a; b.domains[1] = eawr::sim::identity_domain("map-path", "map-b");
    b.domains[2] = eawr::sim::identity_domain("map-digest", "bbb");
    const auto code = eawr::loadcheck::content_code(a);
    if (code.size() != 10 || code != eawr::loadcheck::content_code(b)) return 3;
    b.domains[0] = eawr::sim::identity_domain("rules", "two");
    if (code == eawr::loadcheck::content_code(b)) return 4;
    b = a; b.domains[3] = eawr::sim::identity_domain("units", "changed");
    if (code == eawr::loadcheck::content_code(b)) return 5;
    auto independent = a; independent.domains.erase(independent.domains.begin() + 1, independent.domains.begin() + 3);
    if (code != eawr::loadcheck::content_code(eawr::sim::compute_match_identity(independent))) return 6;
    std::cout << "grouping and map-independent identity passed\n";
}
