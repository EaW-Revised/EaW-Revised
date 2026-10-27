#pragma once
#include "eawr/core/diagnostic.hpp"
#include "eawr/sim/match_identity.hpp"
#include <map>
#include <string>
#include <vector>

namespace eawr::loadcheck {
struct Group { std::size_t count{}; core::Severity severity{core::Severity::info}; std::vector<std::string> examples; };
using Groups = std::map<std::string, Group>;
void add(Groups&, std::string code, std::string example, core::Severity);
std::size_t count(const Groups&, core::Severity);
std::string groups_json(const Groups&);
// Existing match domains, excluding only the map-specific pair; no wire identity changes.
std::string content_code(sim::MatchIdentity);
}
