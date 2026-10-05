#include "ai_data.hpp"

#include <algorithm>
#include <utility>

namespace eawr::script::foc::ai {
namespace {

constexpr std::string_view data_error = "EAWR-AI-0102";

core::Diagnostic error(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(data_error);
    diagnostic.message = std::move(message);
    return diagnostic;
}

bool space(char character) { return character == ' ' || character == '\t' || character == '\r' || character == '\n'; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t index = 0; index < a.size(); ++index) {
        char left = a[index];
        char right = b[index];
        if (left >= 'a' && left <= 'z') left = static_cast<char>(left - ('a' - 'A'));
        if (right >= 'a' && right <= 'z') right = static_cast<char>(right - ('a' - 'A'));
        if (left != right) return false;
    }
    return true;
}

class XmlReader {
public:
    XmlReader(std::string_view path, std::string_view text) : path_(path), text_(text) {}

    core::Result<XmlElement> document() {
        skip_misc();
        auto root = element();
        if (!root) return root;
        return root;
    }

private:
    core::Diagnostic fail(std::string message) const {
        return error(std::string(path_) + ": " + message + " at byte " + std::to_string(cursor_));
    }

    // Comments, declarations and white space between elements.
    void skip_misc() {
        for (;;) {
            while (cursor_ < text_.size() && space(text_[cursor_])) ++cursor_;
            if (text_.compare(cursor_, 4, "<!--") == 0) {
                const std::size_t end = text_.find("-->", cursor_ + 4);
                cursor_ = end == std::string_view::npos ? text_.size() : end + 3;
                continue;
            }
            if (text_.compare(cursor_, 2, "<?") == 0 || text_.compare(cursor_, 2, "<!") == 0) {
                const std::size_t end = text_.find('>', cursor_ + 2);
                cursor_ = end == std::string_view::npos ? text_.size() : end + 1;
                continue;
            }
            return;
        }
    }

    core::Result<XmlElement> element() {
        using ElementResult = core::Result<XmlElement>;
        if (cursor_ >= text_.size() || text_[cursor_] != '<') return ElementResult::failure(fail("expected an element"));
        ++cursor_;
        XmlElement out;
        while (cursor_ < text_.size() && !space(text_[cursor_]) && text_[cursor_] != '>' && text_[cursor_] != '/') {
            out.name.push_back(text_[cursor_++]);
        }
        // Attributes: only Name is kept.
        for (;;) {
            while (cursor_ < text_.size() && space(text_[cursor_])) ++cursor_;
            if (cursor_ >= text_.size()) return ElementResult::failure(fail("unterminated tag"));
            if (text_[cursor_] == '/') {
                if (text_.compare(cursor_, 2, "/>") != 0) return ElementResult::failure(fail("bad empty tag"));
                cursor_ += 2;
                return ElementResult::success(std::move(out));
            }
            if (text_[cursor_] == '>') {
                ++cursor_;
                break;
            }
            std::string attribute;
            while (cursor_ < text_.size() && text_[cursor_] != '=' && !space(text_[cursor_])) attribute.push_back(text_[cursor_++]);
            while (cursor_ < text_.size() && (space(text_[cursor_]) || text_[cursor_] == '=')) ++cursor_;
            if (cursor_ >= text_.size() || (text_[cursor_] != '"' && text_[cursor_] != '\'')) {
                return ElementResult::failure(fail("attribute without a quoted value"));
            }
            const char quote = text_[cursor_++];
            const std::size_t end = text_.find(quote, cursor_);
            if (end == std::string_view::npos) return ElementResult::failure(fail("unterminated attribute"));
            if (iequal(attribute, "Name")) out.name_attribute = std::string(text_.substr(cursor_, end - cursor_));
            cursor_ = end + 1;
        }
        for (;;) {
            if (cursor_ >= text_.size()) return ElementResult::failure(fail("element " + out.name + " is not closed"));
            if (text_.compare(cursor_, 4, "<!--") == 0) {
                const std::size_t end = text_.find("-->", cursor_ + 4);
                cursor_ = end == std::string_view::npos ? text_.size() : end + 3;
                continue;
            }
            if (text_.compare(cursor_, 2, "</") == 0) {
                const std::size_t end = text_.find('>', cursor_);
                if (end == std::string_view::npos) return ElementResult::failure(fail("unterminated end tag"));
                cursor_ = end + 1;
                return ElementResult::success(std::move(out));
            }
            if (text_[cursor_] == '<') {
                auto child = element();
                if (!child) return child;
                out.children.push_back(std::move(child).value());
                continue;
            }
            out.text.push_back(text_[cursor_++]);
        }
    }

    std::string_view path_;
    std::string_view text_;
    std::size_t cursor_{};
};

const std::string* find_file(const std::map<std::string, std::string, std::less<>>& xml, std::string_view path) {
    for (const auto& [name, text] : xml) {
        if (iequal(name, path)) return &text;
    }
    return nullptr;
}

std::optional<Real> number(std::string_view text) { return parse_real(trimmed(text)); }

// A scalar tag of gameconstants.xml, read without parsing the whole file.
std::optional<std::string> constant_tag(std::string_view text, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::size_t start = text.find(open);
    if (start == std::string_view::npos) return std::nullopt;
    const std::size_t end = text.find('<', start + open.size());
    if (end == std::string_view::npos) return std::nullopt;
    return trimmed(text.substr(start + open.size(), end - start - open.size()));
}

GoalType goal_type(const XmlElement& element) {
    GoalType goal;
    goal.name = element.name;
    for (const std::string& flag : split_names(element.child_text("AIGoalApplicationFlags"), "|")) {
        goal.application.insert(upper_case(flag));
    }
    goal.mode = upper_case(element.child_text("GameMode"));
    goal.category = upper_case(element.child_text("Category"));
    goal.reachability = upper_case(element.child_text("Reachability"));
    const auto set = [&](std::string_view tag, Real& field) {
        if (const auto value = number(element.child_text(tag))) field = *value;
    };
    set("Time_Limit", goal.time_limit);
    set("Build_Time_Delay_Tolerance", goal.build_time_delay_tolerance);
    set("Tracking_Duration", goal.tracking_duration);
    set("Per_Failure_Desire_Adjust", goal.per_failure_desire_adjust);
    set("Activation_Tracking_Duration", goal.activation_tracking_duration);
    set("Per_Activation_Failure_Desire_Adjust", goal.per_activation_failure_desire_adjust);
    for (const XmlElement& child : element.children) {
        if (iequal(child.name, "Is_Like")) {
            for (const std::string& like : split_names(child.text, ", \t\r\n")) goal.is_like.push_back(upper_case(like));
        }
    }
    return goal;
}

std::set<std::string> categories(const XmlElement* section, std::string_view tag) {
    std::set<std::string> out;
    if (section == nullptr) return out;
    for (const XmlElement& child : section->children) {
        if (iequal(child.name, tag)) out.insert(upper_case(trimmed(child.text)));
    }
    return out;
}

} // namespace

const XmlElement* XmlElement::child(std::string_view wanted) const {
    for (const XmlElement& element : children) {
        if (iequal(element.name, wanted)) return &element;
    }
    return nullptr;
}

std::string XmlElement::child_text(std::string_view wanted) const {
    const XmlElement* found = child(wanted);
    return found == nullptr ? std::string() : trimmed(found->text);
}

core::Result<XmlElement> parse_xml(std::string_view path, std::string_view text) {
    return XmlReader(path, text).document();
}

std::string upper_case(std::string_view text) {
    std::string out(text);
    for (char& character : out) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    }
    return out;
}

std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return std::string(text.substr(begin, end - begin));
}

std::vector<std::string> split_names(std::string_view text, std::string_view separators) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || separators.find(text[index]) != std::string_view::npos) {
            std::string name = trimmed(text.substr(begin, index - begin));
            if (!name.empty()) out.push_back(std::move(name));
            begin = index + 1;
        }
    }
    return out;
}

std::vector<std::string> ai_xml_files() {
    return {
        "data/xml/ai/players/basicempireplayer.xml",
        "data/xml/ai/players/basicrebelplayer.xml",
        "data/xml/ai/players/ai_player_underworld.xml",
        "data/xml/ai/templates/basicgenerictemplates.xml",
        "data/xml/ai/goalfunctions/basicoffensivespaceset.xml",
        "data/xml/ai/goalfunctions/ai_goalset_underworld_space.xml",
        "data/xml/ai/goalfunctions/systemfunctions.xml",
        "data/xml/ai/goals/offensivespacegoals.xml",
        "data/xml/ai/goals/ai_goals_underworld_space.xml",
        "data/xml/ai/perceptualequations/offensivespaceequations.xml",
        "data/xml/ai/perceptualequations/ai_equations_underworld_space.xml",
        "data/xml/ai/perceptualequations/budgetingequations.xml",
        "data/xml/ai/perceptualequations/basicgalacticequations.xml",
        "data/xml/ai/perceptualequations/basiclandequations.xml",
        "data/xml/ai/perceptualequations/ai_equations_expansiongeneric_landskirmish.xml",
        "data/xml/difficultyadjustments.xml",
        "data/xml/gameconstants.xml",
    };
}

core::Result<AiData> load_ai_data(
    const std::map<std::string, std::string, std::less<>>& xml, const ConverterFunction& converters) {
    using DataResult = core::Result<AiData>;
    AiData data;
    std::vector<std::pair<std::string, std::string>> equation_files;
    for (const std::string& path : ai_xml_files()) {
        const std::string* text = find_file(xml, path);
        if (text == nullptr) return DataResult::failure(error(path + ": AI XML file missing"));
        if (path.find("/perceptualequations/") != std::string::npos) {
            equation_files.emplace_back(path, *text);
            continue;
        }
        if (path.ends_with("gameconstants.xml")) {
            Constants& constants = data.constants;
            const auto set = [&](std::string_view tag, Real& field) {
                if (const auto value = constant_tag(*text, tag)) {
                    if (const auto parsed = number(*value)) field = *parsed;
                }
            };
            set("AI_SpaceEvaluatorRegionSize", constants.region_size);
            set("DesiredSpaceFOWCellSize", constants.fog_cell_size);
            set("AI_SpaceThreatLookAheadTime", constants.threat_look_ahead);
            set("AI_SpaceThreatRangeCap", constants.threat_range_cap);
            set("AI_SpaceAreaThreatScaleFactor", constants.area_threat_scale);
            set("AI_SpaceThreatDecayStep", constants.threat_decay_step);
            set("Health_Low_Percent_Threshold", constants.health_low_threshold);
            Real cells = real(constants.fog_cells_per_threat_cell);
            set("AI_FogCellsPerThreatCell", cells);
            constants.fog_cells_per_threat_cell = std::max<std::int64_t>(1, truncate(cells));
            continue;
        }
        auto document = parse_xml(path, *text);
        if (!document) return DataResult::failure(document.error());
        const XmlElement& root = document.value();
        if (path.find("/players/") != std::string::npos) {
            PlayerType player;
            player.name = root.child_text("Name");
            for (const std::string& set : split_names(root.child_text("GoalProposalFunctionSets"), ", \t\r\n")) {
                player.function_sets.push_back(upper_case(set));
            }
            if (const XmlElement* templates = root.child("Templates")) {
                for (const std::string& name : split_names(templates->child_text("Space"), ", \t\r\n")) {
                    player.space_templates.push_back(upper_case(name));
                }
            }
            if (const XmlElement* adjustments = root.child("Difficulty_Adjustments")) {
                for (const XmlElement& child : adjustments->children) {
                    player.difficulty.emplace(upper_case(child.name), upper_case(trimmed(child.text)));
                }
            }
            data.players.emplace(upper_case(player.name), std::move(player));
        } else if (path.find("/templates/") != std::string::npos) {
            for (const XmlElement& element : root.children) {
                Template entry;
                entry.name = element.name;
                if (const XmlElement* budget = element.child("Budget")) {
                    for (const XmlElement& child : budget->children) {
                        entry.budget.emplace(upper_case(child.name), trimmed(child.text));
                    }
                }
                if (const XmlElement* on = element.child("Turn_On")) {
                    entry.goals_on = categories(on->child("Goals"), "Category");
                    entry.plans_on = categories(on->child("Plans"), "Goal_Category");
                }
                if (const XmlElement* off = element.child("Turn_Off")) {
                    entry.goals_off = categories(off->child("Goals"), "Category");
                    entry.plans_off = categories(off->child("Plans"), "Goal_Category");
                }
                data.templates.emplace(upper_case(entry.name), std::move(entry));
            }
        } else if (path.find("/goalfunctions/") != std::string::npos) {
            // The set is named by its file (basicoffensivespaceset.xml -> BASICOFFENSIVESPACESET).
            std::string name = path.substr(path.rfind('/') + 1);
            name = upper_case(name.substr(0, name.size() - 4));
            std::vector<GoalFunction> functions;
            for (const XmlElement& element : root.children) {
                functions.push_back(GoalFunction{upper_case(element.child_text("Goal")), element.child_text("Function")});
            }
            data.function_sets.emplace(std::move(name), std::move(functions));
        } else if (path.find("/goals/") != std::string::npos) {
            for (const XmlElement& element : root.children) {
                GoalType goal = goal_type(element);
                const std::string key = upper_case(goal.name);
                data.goals.emplace(key, std::move(goal));
            }
        } else if (path.ends_with("difficultyadjustments.xml")) {
            for (const XmlElement& element : root.children) {
                Difficulty difficulty;
                if (const auto value = number(element.child_text("Space_AI_Contrast_Multiplier"))) {
                    difficulty.space_contrast_multiplier = *value;
                }
                if (const auto value = number(element.child_text("Space_AI_Goal_Cycle_Sleep_Duration"))) {
                    difficulty.space_goal_cycle_sleep = *value;
                }
                data.difficulties.emplace(upper_case(element.name_attribute), difficulty);
            }
        }
    }
    auto equations = EquationSet::parse(equation_files, converters);
    if (!equations) return DataResult::failure(equations.error());
    data.equations = std::move(equations).value();
    return DataResult::success(std::move(data));
}

} // namespace eawr::script::foc::ai
