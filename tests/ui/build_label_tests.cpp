// The window title and overlay rows state the viewer build and the engine build, and a
// debug-enabled engine is never hidden. The signature text itself comes from the engine guard
// (check_engine_signature, covered by engine_signature_tests.cpp); here it is only composed.

#include "eawr/presentation/engine_signature.hpp"
#include "eawr/presentation/ui/build_label.hpp"
#include "ui_test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;
namespace pres = eawr::presentation;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

std::string patched_display() {
    return pres::check_engine_signature("eawr", "fad06b579abcdef", "v").display;
}

void test_release_viewer_debug_enabled_editor() {
    const std::string engine = ui::engine_summary(4, 7, 2, ui::engine_build_kind(true, true));
    expect(engine == "4.7.2 editor, debug-enabled", "an editor engine reads debug-enabled");
    expect(ui::window_title("EAWR Viewer", "", "release", engine, patched_display())
               == "EAWR Viewer (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, eawr fad06b579)",
           "release viewer in the editor binary says both and shows no (DEBUG)");
    expect(ui::overlay_viewer_row("release") == "viewer: release" && ui::overlay_engine_row(engine) == "engine: 4.7.2 editor, debug-enabled",
           "overlay rows");
    expect(ui::overlay_signature_row(patched_display()) == "engine: eawr fad06b579", "overlay signature row");
}

void test_release_viewer_release_engine() {
    const std::string engine = ui::engine_summary(4, 7, 2, ui::engine_build_kind(false, false));
    expect(ui::window_title("EAWR Viewer", "", "release", engine, patched_display())
               == "EAWR Viewer (viewer: release; engine: Godot 4.7.2 release template, eawr fad06b579)",
           "a release template engine reads plainly release, no (DEBUG)");
}

void test_debug_viewer_debug_engine() {
    const std::string engine = ui::engine_summary(4, 7, 2, ui::engine_build_kind(false, true));
    expect(engine == "4.7.2 debug template, debug-enabled", "a debug template engine reads debug-enabled");
    expect(ui::window_title("EAWR Viewer", "", "debug", engine, patched_display())
               == "EAWR Viewer (viewer: debug; engine: Godot 4.7.2 debug template, debug-enabled, eawr fad06b579)",
           "debug viewer on a debug engine says both, no (DEBUG)");
}

void test_unpatched_titles() {
    const std::string engine = ui::engine_summary(4, 7, 2, ui::engine_build_kind(true, true));
    const std::string official = pres::check_engine_signature("official", "ed1daf0bf1234", "v").display;
    expect(ui::window_title("EAWR Viewer", "", "release", engine, official)
               == "EAWR Viewer (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, UNPATCHED ENGINE (official))",
           "the official engine's title carries UNPATCHED ENGINE (official)");
    const std::string unverified = pres::check_engine_signature("eawr", "fad06b579abcdef", "v", true).display;
    expect(ui::window_title("EAWR Viewer", "", "release", engine, unverified)
               == "EAWR Viewer (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, UNPATCHED ENGINE (unverified))",
           "an exe the launcher could not verify is UNPATCHED in the title, not 'eawr <hash>'");
    const std::string wrong_hash = pres::check_engine_signature("eawr", "0123456789abc", "v").display;
    expect(ui::overlay_signature_row(wrong_hash) == "engine: UNPATCHED ENGINE (eawr)", "build eawr with a wrong hash is UNPATCHED");
}

void test_profile_and_owned_title() {
    const std::string engine = ui::engine_summary(4, 7, 2, ui::engine_build_kind(true, true));
    const std::string with_profile = ui::window_title("EAWR Viewer", "Thrawn", "release", engine, patched_display());
    expect(with_profile == "EAWR Viewer - Thrawn (viewer: release; engine: Godot 4.7.2 editor, debug-enabled, eawr fad06b579)",
           "a launcher profile name follows the application name");
    expect(ui::window_title("EAWR Viewer", "", "release", engine, patched_display()).find(" - ") == std::string::npos,
           "no profile leaves the title unchanged");
    expect(with_profile.find("DEBUG") == std::string::npos && with_profile.find("(debug)") == std::string::npos,
           "the title never carries the engine's debug suffix");

    std::vector<std::string> written;
    const auto sink = [&](const std::string& text) { written.push_back(text); };
    ui::OwnedTitle owner;
    owner.reapply(sink);
    expect(written.empty(), "nothing is written before a title is set");
    owner.set(with_profile, sink);
    expect(written.size() == 1 && written[0] == with_profile, "set writes the title once");
    owner.reapply(sink);
    owner.reapply(sink);
    expect(written.size() == 3 && written[2] == with_profile, "every engine rewrite is answered with the same title");
}
} // namespace

int main() {
    test_profile_and_owned_title();
    test_release_viewer_debug_enabled_editor();
    test_release_viewer_release_engine();
    test_debug_viewer_debug_engine();
    test_unpatched_titles();
    if (eawr::test::ui::failures() != 0) return 1;
    std::cout << "ui build label passed\n";
    return 0;
}
