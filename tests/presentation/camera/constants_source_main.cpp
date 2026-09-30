#include "eawr/presentation/camera/constants_source.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace camera = eawr::presentation::camera;

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string camera_xml(std::string_view name, std::string_view extra = {}) {
    return "<TacticalCameras><TacticalCamera Name=\"" + std::string(name) + "\">"
        "<Distance_Min>10</Distance_Min><Distance_Max>100</Distance_Max>"
        "<Distance_Default>40</Distance_Default>"
        "<Distance_Per_Mouse_Unit>2</Distance_Per_Mouse_Unit>"
        "<Pitch_Min>10</Pitch_Min><Pitch_Max>80</Pitch_Max>"
        "<Pitch_Default>45</Pitch_Default>"
        "<Near_Clip>1</Near_Clip><Far_Clip>1000</Far_Clip>"
        + std::string(extra) + "</TacticalCamera></TacticalCameras>";
}

std::string scroll_xml() {
    return "<GameConstants>"
        "<Tactical_Min_Scroll_Speed>1</Tactical_Min_Scroll_Speed>"
        "<Tactical_Max_Scroll_Speed>10</Tactical_Max_Scroll_Speed>"
        "<Tactical_Edge_Scroll_Region>2</Tactical_Edge_Scroll_Region>"
        "<Tactical_Offscreen_Scroll_Region>3</Tactical_Offscreen_Scroll_Region>"
        "<Push_Scroll_Speed_Modifier>1.5</Push_Scroll_Speed_Modifier>"
        "<Scroll_Acceleration_Factor>4</Scroll_Acceleration_Factor>"
        "<Scroll_Deceleration_Factor>5</Scroll_Deceleration_Factor>"
        "</GameConstants>";
}

std::span<const std::byte> bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

eawr::core::Result<camera::LoadedConstants> load(const std::string& camera_text,
                                                   const std::string& scroll_text,
                                                   camera::Mode mode = camera::Mode::land) {
    return camera::load_constants(
        {bytes(camera_text), "data/xml/tacticalcameras.xml", "camera-sha"},
        {bytes(scroll_text), "data/xml/gameconstants.xml", "global-sha"}, mode);
}

void rejects(const std::string& camera_text, const std::string& scroll_text,
             std::string_view expected) {
    auto result = load(camera_text, scroll_text);
    expect(!result, std::string("rejected ") + std::string(expected));
    if (!result) expect(result.error().message.find(expected) != std::string::npos,
                        std::string("diagnostic names ") + std::string(expected));
}

constexpr std::string_view override_sha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

camera::OverrideSource override_source(std::string_view authority = "project-authored") {
    return {"map-camera.xml", override_sha, "synthetic-overrides-v1", authority};
}

eawr::core::Result<camera::LoadedConstants> apply(
    const camera::LoadedConstants& base, std::vector<camera::ConstantOverride> overrides,
    camera::OverrideSource source = override_source()) {
    return camera::apply_map_overrides(base, source, overrides);
}

void rejects_override(const camera::LoadedConstants& base,
                      std::vector<camera::ConstantOverride> overrides,
                      std::string_view expected, std::string_view name,
                      camera::OverrideSource source = override_source(),
                      std::string_view code = camera::invalid_override) {
    auto result = apply(base, std::move(overrides), source);
    expect(!result, std::string("override rejected: ") + std::string(name));
    if (!result) {
        expect(result.error().code == code && result.error().message.find(expected) != std::string::npos,
               std::string("override diagnostic: ") + std::string(name));
    }
}

void test_map_overrides(const std::string& global) {
    auto loaded = load(camera_xml("Land_Mode", "<Yaw_Per_Mouse_Unit>2</Yaw_Per_Mouse_Unit>"), global);
    expect(static_cast<bool>(loaded), "override base loads");
    if (!loaded) return;
    const camera::LoadedConstants base = loaded.value();
    expect(base.overrides.empty(), "load_constants never produces an override layer");

    // Positive: a required camera field, an absent optional field and a global
    // scroll field, each recorded with the replaced value and both sources.
    auto applied = apply(base, {{"Distance_Max", "80"}, {"Fov_Default", "55"},
                                {"Tactical_Max_Scroll_Speed", " 20\t"}});
    expect(static_cast<bool>(applied), "valid overrides apply");
    if (applied) {
        const auto& value = applied.value();
        expect(value.constants.distance_max == 80.0F && value.constants.fov_default == 55.0F
               && value.constants.tactical_max_scroll_speed == 20.0F, "override values replace the XML layer");
        expect(value.constants.distance_min == 10.0F && value.constants.yaw_per_mouse_unit == 2.0F,
               "fields without an override keep the XML value");
        expect(value.provenance.size() == base.provenance.size()
               && value.provenance[1].tag == "Distance_Max"
               && value.provenance[1].file == "data/xml/tacticalcameras.xml",
               "XML provenance rows stay the XML layer");
        expect(value.overrides.size() == 3, "every override is recorded");
        if (value.overrides.size() == 3) {
            const auto& first = value.overrides[0];
            expect(first.tag == "Distance_Max" && first.base_value == 100.0F && first.value == 80.0F
                   && first.base.file == "data/xml/tacticalcameras.xml"
                   && first.base.source_sha256 == "camera-sha" && first.base.definition == "Land_Mode"
                   && first.base.status == camera::FieldStatus::supplied,
                   "override records the replaced XML value and its provenance");
            expect(first.file == "map-camera.xml" && first.source_sha256 == override_sha
                   && first.source_id == "synthetic-overrides-v1" && first.authority == "project-authored",
                   "override records its own source");
            expect(value.overrides[1].base.status == camera::FieldStatus::absent
                   && value.overrides[1].base_value == 0.0F,
                   "an override of an absent optional tag records the inert base");
            expect(value.overrides[2].base.file == "data/xml/gameconstants.xml"
                   && value.overrides[2].base.source_sha256 == "global-sha"
                   && value.overrides[2].base_value == 10.0F, "global scroll override records its XML source");
        }
    }
    expect(base.constants.distance_max == 100.0F && base.overrides.empty(), "the base layer is never mutated");

    // Negative: authority, source identity and shape.
    rejects_override(base, {{"Distance_Max", "80"}}, "project-authored", "original authority",
                     override_source("original"));
    rejects_override(base, {{"Distance_Max", "80"}}, "project-authored", "retail authority",
                     override_source("retail-observed"));
    rejects_override(base, {{"Distance_Max", "80"}}, "logical path", "absolute source path",
                     {"C:/maps/map-camera.xml", override_sha, "id", "project-authored"});
    rejects_override(base, {{"Distance_Max", "80"}}, "SHA-256", "uppercase digest",
                     {"map-camera.xml", "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
                      "id", "project-authored"});
    rejects_override(base, {{"Distance_Max", "80"}}, "SHA-256", "short digest",
                     {"map-camera.xml", "abc", "id", "project-authored"});
    rejects_override(base, {{"Distance_Max", "80"}}, "source ID", "empty source ID",
                     {"map-camera.xml", override_sha, "", "project-authored"});
    rejects_override(base, {}, "at least one", "empty override list");

    // Negative: vocabulary, duplicates and values.
    rejects_override(base, {{"Distance_Max", "80"}, {"Distance_Max", "90"}}, "more than once", "duplicate tag");
    rejects_override(base, {{"Distance_Max", "80"}, {"distance_max", "90"}}, "more than once",
                     "case-folded duplicate tag");
    rejects_override(base, {{"distance_max", "80"}}, "exact spelling", "non-canonical spelling");
    rejects_override(base, {{"Camera_Height", "80"}}, "not a scalar camera constant", "unknown tag");
    for (const std::string_view spline : {"Use_Splines", "Distance_Spline", "Pitch_Spline"}) {
        rejects_override(base, {{std::string(spline), "1"}}, "not overridable", "spline override refused");
    }
    for (const std::string_view lock : {"Land_Tactical_Camera_Locked", "Space_Tactical_Camera_Locked"}) {
        rejects_override(base, {{std::string(lock), "1"}}, "camera-lock", "camera-lock override refused");
    }
    for (const std::string_view bad : {"", "nan", "inf", "12x", "1 2", "1e999"}) {
        rejects_override(base, {{"Distance_Max", std::string(bad)}}, "finite decimal", "malformed value");
    }
    // A result the camera model refuses is rejected with the model's diagnostic.
    rejects_override(base, {{"Distance_Max", "5"}}, "Distance_Max must exceed", "inverted distance range",
                     override_source(), camera::diagnostic_codes::invalid_constants);
    rejects_override(base, {{"Near_Clip", "2000"}}, "Near_Clip", "near beyond far",
                     override_source(), camera::diagnostic_codes::invalid_constants);

    // Scroll speeds (XML 1..10): project override policy, EAWR-CAMERA-0005.
    rejects_override(base, {{"Tactical_Min_Scroll_Speed", "-1"}},
                     "<Tactical_Min_Scroll_Speed> must not be negative", "negative min scroll speed");
    rejects_override(base, {{"Tactical_Max_Scroll_Speed", "-0.5"}},
                     "<Tactical_Max_Scroll_Speed> must not be negative", "negative max scroll speed");
    rejects_override(base, {{"Tactical_Min_Scroll_Speed", "-20"}, {"Tactical_Max_Scroll_Speed", "-10"}},
                     "<Tactical_Min_Scroll_Speed> must not be negative", "ordered negative scroll pair");
    constexpr std::string_view inverted =
        "overridden Tactical_Max_Scroll_Speed must not be below Tactical_Min_Scroll_Speed";
    rejects_override(base, {{"Tactical_Max_Scroll_Speed", "0.5"}}, inverted, "max override below XML min");
    rejects_override(base, {{"Tactical_Min_Scroll_Speed", "11"}}, inverted, "min override above XML max");
    rejects_override(base, {{"Tactical_Min_Scroll_Speed", "8"}, {"Tactical_Max_Scroll_Speed", "4"}},
                     inverted, "inverted scroll override pair");
    auto still = apply(base, {{"Tactical_Min_Scroll_Speed", "0"}, {"Tactical_Max_Scroll_Speed", "0"}});
    expect(still && still.value().constants.tactical_min_scroll_speed == 0.0F
           && still.value().constants.tactical_max_scroll_speed == 0.0F,
           "zero scroll-speed overrides (a stationary pan) apply");
    auto equal = apply(base, {{"Tactical_Min_Scroll_Speed", "10"}});
    expect(equal && equal.value().constants.tactical_min_scroll_speed == 10.0F,
           "a min override equal to the XML max applies");
    // The XML layer keeps validate's own domain: no invented floor for it, and
    // the override rules judge override values and the layered pair only.
    std::string negative_global = global;
    negative_global.replace(negative_global.find(">1<"), 3, ">-5<");
    auto negative_xml = load(camera_xml("Land_Mode"), negative_global);
    expect(negative_xml && negative_xml.value().constants.tactical_min_scroll_speed == -5.0F,
           "a negative XML min scroll speed still loads unchanged");
    if (negative_xml) {
        auto unrelated = apply(negative_xml.value(), {{"Far_Clip", "900"}});
        expect(unrelated && unrelated.value().constants.tactical_min_scroll_speed == -5.0F,
               "an unrelated override leaves the XML scroll speeds as loaded");
        auto raised = apply(negative_xml.value(), {{"Tactical_Max_Scroll_Speed", "20"}});
        expect(raised && raised.value().constants.tactical_max_scroll_speed == 20.0F,
               "a non-negative max override over a negative XML min applies");
    }
    // A second layer on top of an overridden set is refused: one map layer.
    if (applied) {
        rejects_override(applied.value(), {{"Distance_Min", "20"}}, "already carry", "double override layer");
    }
}

} // namespace

int main() {
    const std::string global = scroll_xml();
    for (const camera::Mode mode : {camera::Mode::land, camera::Mode::space,
                                    camera::Mode::unlocked}) {
        const std::string xml = camera_xml(camera::definition_name(mode));
        auto result = load(xml, global, mode);
        expect(static_cast<bool>(result), "each selected camera mode loads");
        if (!result) continue;
        const auto& value = result.value();
        expect(value.constants.distance_default == 40.0F, "selected definition values load");
        expect(value.constants.yaw_per_mouse_unit == 0.0F && !value.constants.use_splines,
               "absent optional values stay inert");
        expect(value.provenance.size() == 36, "provenance covers every consumed field");
        expect(value.provenance[0].tag == "Distance_Min"
               && value.provenance[0].status == camera::FieldStatus::supplied,
               "required field provenance order and status");
        expect(value.provenance[9].tag == "Distance_Smooth_Time"
               && value.provenance[9].status == camera::FieldStatus::absent,
               "optional absence is distinguished from supplied zero");
        expect(value.provenance[22].tag == "Location_Follows_Terrain"
               && value.provenance[25].tag == "Min_Height_Above_Terrain"
               && value.provenance[25].status == camera::FieldStatus::absent
               && value.constants.location_follows_terrain == 0.0F,
               "terrain-following fields are optional and stay inert when absent");
        expect(value.provenance[29].tag == "Tactical_Min_Scroll_Speed"
               && value.provenance[29].source_sha256 == "global-sha",
               "global provenance retains caller digest");
        for (const auto& item : value.provenance) {
            expect(item.file.find(":") == std::string::npos && !item.file.empty()
                   && item.file.front() != '/' && item.file.front() != '\\',
                   "provenance has logical paths only");
        }
    }

    auto following = load(camera_xml("Land_Mode",
        "<Location_Follows_Terrain>1</Location_Follows_Terrain>"
        "<Location_Height_Up_Smooth_Time>0.3</Location_Height_Up_Smooth_Time>"
        "<Location_Height_Down_Smooth_Time>1.0</Location_Height_Down_Smooth_Time>"
        "<Min_Height_Above_Terrain>20.0</Min_Height_Above_Terrain>"), global, camera::Mode::land);
    expect(following && following.value().constants.location_follows_terrain == 1.0F
           && following.value().constants.location_height_up_smooth_time == 0.3F
           && following.value().constants.location_height_down_smooth_time == 1.0F
           && following.value().constants.min_height_above_terrain == 20.0F
           && following.value().provenance[22].status == camera::FieldStatus::supplied,
           "terrain-following fields load from the selected definition");

    const std::string space_xml = camera_xml("Space_Mode");
    const std::string land_xml = camera_xml("Land_Mode", "<Yaw_Default>12</Yaw_Default>");
    auto selected = load(space_xml.substr(0, space_xml.rfind("</TacticalCameras>"))
        + land_xml.substr(land_xml.find("<TacticalCamera ")), global);
    expect(static_cast<bool>(selected) && selected.value().constants.yaw_default == 12.0F,
           "nonmatching definition is ignored");

    for (const std::string_view bad : {"", " ", "oops", "NaN", "inf", "1junk", "1 2", "1e999"}) {
        rejects(camera_xml("Land_Mode", "<Yaw_Default>" + std::string(bad)
                           + "</Yaw_Default>"), global, "Yaw_Default");
    }
    rejects(camera_xml("Land_Mode", "<Yaw_Default>12<![CDATA[junk]]></Yaw_Default>"),
            global, "Yaw_Default");
    rejects(camera_xml("Land_Mode", "<Yaw_Default>12<Extra/></Yaw_Default>"),
            global, "Yaw_Default");
    rejects(camera_xml("Land_Mode", "<Use_Splines>no<![CDATA[maybe]]></Use_Splines>"),
            global, "Use_Splines");
    rejects(camera_xml("Land_Mode", "<Use_Splines>yes</Use_Splines>"
                       "<Distance_Spline>0,10 1,100<![CDATA[junk]]></Distance_Spline>"
                       "<Pitch_Spline>0,10 1,80</Pitch_Spline>"), global, "Distance_Spline");
    rejects(camera_xml("Land_Mode", "<Use_Splines>maybe</Use_Splines>"), global,
            "Use_Splines");
    rejects(camera_xml("Land_Mode", "<Use_Splines/>"), global, "Use_Splines");
    for (const std::string_view flag : {"no", " \tFaLsE\r\n ", "0"}) {
        auto bool_false = load(camera_xml("Land_Mode", "<Use_Splines>" + std::string(flag)
            + "</Use_Splines>"), global);
        expect(static_cast<bool>(bool_false) && !bool_false.value().constants.use_splines,
               "false token trims policy whitespace and folds case");
    }
    auto disabled_curves = load(camera_xml("Land_Mode", "<Use_Splines>no</Use_Splines>"
        "<Distance_Spline>invalid but inactive</Distance_Spline>"), global);
    expect(static_cast<bool>(disabled_curves)
           && disabled_curves.value().constants.distance_spline.empty(),
           "inactive spline text retains prior semantics");
    for (const std::string_view flag : {"yes", "TRUE", "1"}) {
        auto curves = load(camera_xml("Land_Mode", "<Use_Splines>" + std::string(flag)
            + "</Use_Splines><Distance_Spline>0,10 1,100</Distance_Spline>"
              "<Pitch_Spline>0,10 1,80</Pitch_Spline>"), global);
        expect(static_cast<bool>(curves) && curves.value().constants.use_splines
               && curves.value().constants.pitch_spline.size() == 2,
               "true token loads both splines");
    }
    rejects(camera_xml("Land_Mode", "<Use_Splines>yes</Use_Splines>"), global,
            "Distance_Spline");
    rejects(camera_xml("Land_Mode", "<Use_Splines>yes</Use_Splines>"
                       "<Distance_Spline>0,10 bad</Distance_Spline>"
                       "<Pitch_Spline>0,10 1,80</Pitch_Spline>"), global,
            "Distance_Spline");
    rejects("<TacticalCameras><TacticalCamera Name=\"Land_Mode\">"
            "<Distance_Max>100</Distance_Max></TacticalCamera></TacticalCameras>", global,
            "Distance_Min");
    rejects(camera_xml("Land_Mode"), "<GameConstants/>", "Tactical_Min_Scroll_Speed");
    rejects(camera_xml("Land_Mode", "<Yaw_Min>1</Yaw_Min><yaw_min>1</yaw_min>"),
            global, "Yaw_Min");
    rejects(camera_xml("Land_Mode", "<Use_Splines>no</Use_Splines>"
                       "<use_splines>no</use_splines>"), global, "Use_Splines");
    rejects(camera_xml("Land_Mode", "<Distance_Spline>0,10 1,100</Distance_Spline>"
                       "<distance_spline>0,10 1,100</distance_spline>"), global,
            "Distance_Spline");
    rejects(camera_xml("Land_Mode"), global.substr(0, global.rfind("</GameConstants>"))
            + "<Tactical_Min_Scroll_Speed>1</Tactical_Min_Scroll_Speed></GameConstants>",
            "Tactical_Min_Scroll_Speed");
    const std::string base = camera_xml("Land_Mode");
    const std::string duplicate = base.substr(0, base.rfind("</TacticalCameras>"))
        + "<TacticalCamera Name=\"land_mode\"/></TacticalCameras>";
    rejects(duplicate, global, "duplicate");

    const std::string xml = camera_xml("Land_Mode");
    auto absolute = camera::load_constants({bytes(xml), "C:/host/tacticalcameras.xml", "sha"},
                                          {bytes(global), "data/xml/gameconstants.xml", "sha"},
                                          camera::Mode::land);
    expect(!absolute && absolute.error().message.find("relative logical paths")
           != std::string::npos, "absolute source path rejected");

    test_map_overrides(global);

    if (failures) std::cerr << failures << " camera source contract failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
