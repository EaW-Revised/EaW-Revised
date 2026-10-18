find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Public documentation describes work; tracker numbers are explicit legacy references.
add_test(NAME ci_doc_refs COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/publish_scan/doc_refs.py"
         --root "${PROJECT_SOURCE_DIR}")
add_test(NAME ci_doc_refs_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_doc_refs.py")
# The replay number registry (docs/registries/replay-numbers.json): doc, registry and code constants agree.
add_test(NAME ci_replay_numbers COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_replay_numbers.py"
         --root "${PROJECT_SOURCE_DIR}")
add_test(NAME ci_replay_numbers_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_replay_numbers.py")

add_test(NAME ci_native_cpp_fixture COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_native_cpp_fixture.py")
add_test(NAME ci_game_data_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_game_data_tests.py")

# The XML tag registry gate (docs/tag-coverage.md): every (class, tag) of the FoC XML has a row with what its status
# needs, every applied code location exists, the JSON and its rendering are canonical. Without game data; the
# game-data half (tag_registry_game_data) sits with the inventory tests.
add_test(
    NAME ci_tag_registry
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/inventory/tag_registry.py" check
            --repo "${PROJECT_SOURCE_DIR}"
)
add_test(NAME ci_tag_registry_no_targets COMMAND "${Python3_EXECUTABLE}"
         "${CMAKE_CURRENT_SOURCE_DIR}/test_tag_registry_no_targets.py")

# The XML literal gate (docs/tag-coverage.md): source code must not name a FoC XML tag next to a numeric literal, except
# a documented `xml-fallback:` default or an entry of config/xml-literal-allowlist.json (which may only shrink).
add_test(
    NAME ci_xml_literal_gate
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/inventory/xml_literal_check.py" check
            --repo "${PROJECT_SOURCE_DIR}" --base-ref origin/Thom-Ernst/Skirmish
)
set_tests_properties(ci_xml_literal_gate PROPERTIES LABELS "ci-tools")
add_test(NAME ci_xml_literal_check COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_xml_literal_check.py")
set_tests_properties(ci_xml_literal_check PROPERTIES LABELS "ci-tools")

# #527: the shared evidence-read retry guard, and that every tools/compare_*.py comparator uses it.
add_test(
    NAME ci_evidence_io
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_evidence_io.py"
)

# #825: no FoC engine class/method names in tracked files (tools/cleanroom_check.py).
add_test(
    NAME ci_cleanroom_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cleanroom_check.py"
)

# The publish scan the public tree must pass (tools/publish_scan; the public CI runs it over the tree).
add_test(
    NAME ci_publish_scan
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_publish_scan.py"
)

# The release packager (tools/release, docs/releasing.md) on a stand-in build tree.


# Contracts added by the melee, menu, minimap and galactic HUD work.
eawr_add_python_contract(python_performance_melee_parity_analyze tests.performance.test_melee_parity_analyze)
eawr_add_python_contract(python_performance_melee_staging tests.performance.test_melee_staging)
eawr_add_python_contract(python_presentation_renderer_menu_mod_layouts tests.presentation.renderer.test_menu_mod_layouts)

eawr_add_python_contract(python_presentation_renderer_viewer_galactic_fleet_stack_popup_runtime tests.presentation.renderer.test_viewer_galactic_fleet_stack_popup_runtime)
eawr_add_python_contract(python_presentation_renderer_viewer_galactic_planet_tooltips_runtime tests.presentation.renderer.test_viewer_galactic_planet_tooltips_runtime)

eawr_add_python_contract(python_presentation_renderer_viewer_galactic_orbit_runtime tests.presentation.renderer.test_viewer_galactic_orbit_runtime)

# Tier labels (cmake/EawrTestTiers.cmake).
add_test(NAME ci_campaign_policy_compare COMMAND "${Python3_EXECUTABLE}"
         "${CMAKE_CURRENT_SOURCE_DIR}/test_campaign_policy_compare.py")






eawr_label_tests(DEFAULT ci-tools)
