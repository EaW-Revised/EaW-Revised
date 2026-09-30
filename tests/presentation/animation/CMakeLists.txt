if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(eawr_animation_cpu_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_executable(animation_cpu_tests
        main.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    )
    add_executable(animation_idle_playback_tests
        idle_playback_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
        "${EAWR_ROOT}/src/presentation/animation/idle_playback.cpp"
    )
    target_include_directories(animation_idle_playback_tests PRIVATE "${EAWR_ROOT}/include")
    if(MSVC)
        target_compile_options(animation_idle_playback_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(animation_idle_playback_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_executable(animation_corpus_playback
        corpus_playback.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/vfs/vfs.cpp"
        "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
        "${EAWR_ROOT}/src/assets/model.cpp"
        "${EAWR_ROOT}/src/assets/animation.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    )
    add_executable(animation_corpus_validation
        corpus_validation.cpp
        corpus_validation_checks.cpp
        corpus_validation_load.cpp
        corpus_validation_report.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/core/sha256.cpp"
        "${EAWR_ROOT}/src/vfs/vfs.cpp"
        "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
        "${EAWR_ROOT}/src/assets/model.cpp"
        "${EAWR_ROOT}/src/assets/animation.cpp"
        "${EAWR_ROOT}/src/sim/replay.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    )
    add_executable(animation_corpus_diagnostics_tests
        corpus_diagnostics_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    )
    add_executable(animation_corpus_associations_tests
        corpus_associations_tests.cpp
        corpus_associations_baseline_tests.cpp
        corpus_associations_candidate_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
    )
    # The declaration probe reads the effective XML catalog, whose loader needs
    # the generated schema contract.
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    set(EAWR_XML_SCHEMA_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/xml_schema_contract.inc")
    add_custom_command(
        OUTPUT "${EAWR_XML_SCHEMA_INC}"
        COMMAND "${Python3_EXECUTABLE}" "${EAWR_ROOT}/src/data/generate_schema_contract.py"
            --input "${EAWR_ROOT}/plan/inventories/xml-tags.json" --output "${EAWR_XML_SCHEMA_INC}"
        DEPENDS "${EAWR_ROOT}/src/data/generate_schema_contract.py" "${EAWR_ROOT}/plan/inventories/xml-tags.json"
        VERBATIM)
    set(EAWR_DECLARATION_SOURCES
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/vfs/vfs.cpp"
        "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
        "${EAWR_ROOT}/src/data/xml.cpp"
        "${EAWR_ROOT}/src/data/xml_merge.cpp"
        "${EAWR_ROOT}/src/data/xml_registry.cpp"
        "${EAWR_XML_SCHEMA_INC}"
        "${EAWR_ROOT}/src/sim/replay.cpp"
        "${EAWR_ROOT}/src/sim/math/fixed.cpp"
        "${EAWR_ROOT}/src/sim/math/wide.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp")
    add_executable(animation_corpus_declarations_tests corpus_declarations_tests.cpp ${EAWR_DECLARATION_SOURCES})
    add_executable(animation_corpus_declarations corpus_declarations.cpp ${EAWR_DECLARATION_SOURCES})
    # The structural-lead declaration probe reads the same catalog.
    add_executable(animation_corpus_structural_declaration_tests corpus_structural_declaration_tests.cpp
        ${EAWR_DECLARATION_SOURCES})
    add_executable(animation_corpus_structural_declaration corpus_structural_declaration.cpp ${EAWR_DECLARATION_SOURCES})
    foreach(target IN ITEMS animation_corpus_declarations_tests animation_corpus_declarations
            animation_corpus_structural_declaration_tests animation_corpus_structural_declaration)
        target_include_directories(${target} PRIVATE "${EAWR_ROOT}/include" "${EAWR_ROOT}/src/sim"
            "${EAWR_ROOT}/third_party/pugixml/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()
    # The deformation probe is Godot-free test/evidence tooling.
    add_executable(animation_deformation_probe_tests deformation_probe_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp")
    add_executable(animation_deformation_probe deformation_probe.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/vfs/vfs.cpp"
        "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
        "${EAWR_ROOT}/src/assets/model.cpp"
        "${EAWR_ROOT}/src/assets/animation.cpp"
        "${EAWR_ROOT}/src/sim/replay.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp")
    # Shot-readiness census/palette/attachment helpers and their real-sample probe.
    add_executable(animation_shot_readiness_tests shot_readiness_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/core/sha256.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
        "${EAWR_ROOT}/src/presentation/animation/shot_readiness.cpp")
    add_executable(animation_shot_readiness_probe shot_readiness_probe.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/core/sha256.cpp"
        "${EAWR_ROOT}/src/vfs/vfs.cpp"
        "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
        "${EAWR_ROOT}/src/assets/model.cpp"
        "${EAWR_ROOT}/src/assets/animation.cpp"
        "${EAWR_ROOT}/src/sim/replay.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
        "${EAWR_ROOT}/src/presentation/animation/shot_readiness.cpp")
    # The exact-source diagnosis reads isolated VFS views and parses ALO/ALA.
    set(EAWR_SOURCE_VERSION_SOURCES
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/core/sha256.cpp"
        "${EAWR_ROOT}/src/vfs/vfs.cpp"
        "${EAWR_ROOT}/src/vfs/vfs_archive.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
        "${EAWR_ROOT}/src/assets/model.cpp"
        "${EAWR_ROOT}/src/assets/animation.cpp"
        "${EAWR_ROOT}/src/sim/replay.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp")
    add_executable(animation_corpus_source_versions_tests corpus_source_versions_tests.cpp ${EAWR_SOURCE_VERSION_SOURCES})
    add_executable(animation_corpus_source_versions corpus_source_versions.cpp ${EAWR_SOURCE_VERSION_SOURCES})
    # The structural discovery reads the effective VFS and parses ALO/ALA.
    add_executable(animation_corpus_structural_discovery_tests corpus_structural_discovery_tests.cpp ${EAWR_SOURCE_VERSION_SOURCES})
    add_executable(animation_corpus_structural_discovery corpus_structural_discovery.cpp ${EAWR_SOURCE_VERSION_SOURCES})
    # The model_match discovery reuses the structural-discovery inventory.
    add_executable(animation_corpus_model_match_discovery_tests corpus_model_match_discovery_tests.cpp ${EAWR_SOURCE_VERSION_SOURCES})
    add_executable(animation_corpus_model_match_discovery corpus_model_match_discovery.cpp ${EAWR_SOURCE_VERSION_SOURCES})
    foreach(target IN ITEMS animation_deformation_probe_tests animation_deformation_probe
            animation_shot_readiness_tests animation_shot_readiness_probe
            animation_corpus_source_versions_tests animation_corpus_source_versions
            animation_corpus_structural_discovery_tests animation_corpus_structural_discovery
            animation_corpus_model_match_discovery_tests animation_corpus_model_match_discovery)
        target_include_directories(${target} PRIVATE "${EAWR_ROOT}/include" "${EAWR_ROOT}/third_party/pugixml/src")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()
    # Content identity of the sources that define the corpus receipt. The
    # configure dependency re-hashes them whenever any of them is edited.
    set(EAWR_CORPUS_TOOL_SOURCES
        "tests/presentation/animation/corpus_validation.cpp"
        "tests/presentation/animation/corpus_validation_checks.cpp"
        "tests/presentation/animation/corpus_validation_load.cpp"
        "tests/presentation/animation/corpus_validation_report.cpp"
        "tests/presentation/animation/corpus_validation_support.hpp"
        "tests/presentation/animation/corpus_diagnostics.hpp"
        "tests/presentation/animation/corpus_associations.hpp"
        "tests/presentation/animation/corpus_associations_baseline.hpp"
        "tests/presentation/animation/corpus_associations_candidates.hpp"
        "tests/presentation/animation/corpus_association_frozen.tsv"
        "tests/presentation/animation/corpus_association_frozen.py"
        "src/presentation/animation/animation.cpp"
        "src/assets/animation.cpp"
        "src/assets/model.cpp"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_CORPUS_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_CORPUS_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_CORPUS_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_CORPUS_TOOL_SOURCES EAWR_CORPUS_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/corpus_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_corpus_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_CORPUS_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_CORPUS_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_cpu_tests PRIVATE "${EAWR_ROOT}/include")
    target_include_directories(animation_corpus_diagnostics_tests PRIVATE "${EAWR_ROOT}/include")
    target_include_directories(animation_corpus_associations_tests PRIVATE "${EAWR_ROOT}/include")
    target_compile_definitions(animation_corpus_associations_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv")
    target_include_directories(animation_corpus_validation PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
    target_include_directories(animation_corpus_playback PRIVATE "${EAWR_ROOT}/include" "${EAWR_ROOT}/third_party/pugixml/src")
    target_include_directories(animation_corpus_validation PRIVATE "${EAWR_ROOT}/include" "${EAWR_ROOT}/third_party/pugixml/src")
    if(MSVC)
        target_compile_options(animation_cpu_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(animation_corpus_diagnostics_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(animation_corpus_associations_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(animation_corpus_playback PRIVATE /W4 /WX /permissive-)
        target_compile_options(animation_corpus_validation PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(animation_cpu_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(animation_corpus_diagnostics_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(animation_corpus_associations_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(animation_corpus_playback PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(animation_corpus_validation PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    add_executable(animation_cpu_tests main.cpp)
    target_link_libraries(animation_cpu_tests PRIVATE eawr::presentation)
    target_compile_features(animation_cpu_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(animation_cpu_tests)
    add_executable(animation_idle_playback_tests idle_playback_tests.cpp)
    target_link_libraries(animation_idle_playback_tests PRIVATE eawr::presentation)
    target_compile_features(animation_idle_playback_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(animation_idle_playback_tests)
    add_executable(animation_unit_clips_tests unit_clips_tests.cpp)
    target_link_libraries(animation_unit_clips_tests PRIVATE eawr::presentation)
    target_compile_features(animation_unit_clips_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(animation_unit_clips_tests)
    # #81: the FoC disposition of the frozen association failures (reads the installed game).
    add_executable(animation_corpus_association_foc corpus_association_foc.cpp)
    target_link_libraries(animation_corpus_association_foc PRIVATE eawr::presentation eawr::assets eawr::vfs eawr::data eawr::core)
    target_compile_features(animation_corpus_association_foc PRIVATE cxx_std_20)
    eawr_target_project_warnings(animation_corpus_association_foc)
    add_executable(animation_corpus_diagnostics_tests corpus_diagnostics_tests.cpp)
    target_link_libraries(animation_corpus_diagnostics_tests PRIVATE eawr::presentation)
    target_compile_features(animation_corpus_diagnostics_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(animation_corpus_diagnostics_tests)
    add_executable(animation_corpus_associations_tests
        corpus_associations_tests.cpp
        corpus_associations_baseline_tests.cpp
        corpus_associations_candidate_tests.cpp)
    target_link_libraries(animation_corpus_associations_tests PRIVATE eawr::presentation)
    target_compile_features(animation_corpus_associations_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(animation_corpus_associations_tests)
    target_compile_definitions(animation_corpus_associations_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv")
    add_executable(animation_deformation_probe_tests deformation_probe_tests.cpp)
    add_executable(animation_deformation_probe deformation_probe.cpp)
    add_executable(animation_shot_readiness_tests shot_readiness_tests.cpp)
    add_executable(animation_shot_readiness_probe shot_readiness_probe.cpp)
    add_executable(animation_corpus_source_versions_tests corpus_source_versions_tests.cpp)
    add_executable(animation_corpus_source_versions corpus_source_versions.cpp)
    add_executable(animation_corpus_structural_discovery_tests corpus_structural_discovery_tests.cpp)
    add_executable(animation_corpus_structural_discovery corpus_structural_discovery.cpp)
    add_executable(animation_corpus_model_match_discovery_tests corpus_model_match_discovery_tests.cpp)
    add_executable(animation_corpus_model_match_discovery corpus_model_match_discovery.cpp)
    foreach(target IN ITEMS animation_deformation_probe_tests animation_deformation_probe
            animation_shot_readiness_tests animation_shot_readiness_probe
            animation_corpus_source_versions_tests animation_corpus_source_versions
            animation_corpus_structural_discovery_tests animation_corpus_structural_discovery
            animation_corpus_model_match_discovery_tests animation_corpus_model_match_discovery)
        target_link_libraries(${target} PRIVATE eawr::presentation eawr::assets eawr::vfs eawr::sim)
        target_compile_features(${target} PRIVATE cxx_std_20)
        eawr_target_project_warnings(${target})
    endforeach()
    # The declaration probe reads the effective XML catalog.
    set(EAWR_ROOT "${PROJECT_SOURCE_DIR}")
    add_executable(animation_corpus_declarations_tests corpus_declarations_tests.cpp)
    add_executable(animation_corpus_declarations corpus_declarations.cpp)
    add_executable(animation_corpus_structural_declaration_tests corpus_structural_declaration_tests.cpp)
    add_executable(animation_corpus_structural_declaration corpus_structural_declaration.cpp)
    foreach(target IN ITEMS animation_corpus_declarations_tests animation_corpus_declarations
            animation_corpus_structural_declaration_tests animation_corpus_structural_declaration)
        target_link_libraries(${target} PRIVATE eawr::presentation eawr::assets eawr::data eawr::vfs eawr::sim)
        target_compile_features(${target} PRIVATE cxx_std_20)
        eawr_target_project_warnings(${target})
    endforeach()
endif()

if(TARGET animation_corpus_declarations)
    target_compile_definitions(animation_corpus_declarations_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        EAWR_DECLARATION_PAIRS="${CMAKE_CURRENT_SOURCE_DIR}/corpus_declaration_pairs.tsv")
    # Content identity of the sources that define the declaration receipt.
    set(EAWR_DECLARATION_TOOL_SOURCES
        "tests/presentation/animation/corpus_declarations.cpp"
        "tests/presentation/animation/corpus_declarations.hpp"
        "tests/presentation/animation/corpus_declarations_land.hpp"
        "tests/presentation/animation/corpus_declarations_other.hpp"
        "tests/presentation/animation/corpus_declarations_space.hpp"
        "tests/presentation/animation/corpus_declaration_pairs.tsv"
        "tests/presentation/animation/corpus_declaration_pairs.py"
        "tests/presentation/animation/corpus_associations.hpp"
        "tests/presentation/animation/corpus_associations_baseline.hpp"
        "tests/presentation/animation/corpus_associations_candidates.hpp"
        "tests/presentation/animation/corpus_diagnostics.hpp"
        "tests/presentation/animation/corpus_association_frozen.tsv"
        "include/eawr/data/xml.hpp"
        "src/data/xml.cpp"
        "src/data/xml_merge.cpp"
        "src/data/xml_registry.cpp"
        "plan/inventories/xml-tags.json"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_DECLARATION_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_DECLARATION_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_DECLARATION_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_DECLARATION_TOOL_SOURCES EAWR_DECLARATION_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/declaration_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_declaration_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_DECLARATION_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_DECLARATION_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_corpus_declarations PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
endif()

if(TARGET animation_corpus_structural_declaration)
    target_compile_definitions(animation_corpus_structural_declaration_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        EAWR_DECLARATION_PAIRS="${CMAKE_CURRENT_SOURCE_DIR}/corpus_declaration_pairs.tsv")
    # Content identity of the sources that define the structural-declaration receipt.
    set(EAWR_STRUCTURAL_DECLARATION_TOOL_SOURCES
        "tests/presentation/animation/corpus_structural_declaration.cpp"
        "tests/presentation/animation/corpus_structural_declaration.hpp"
        "tests/presentation/animation/corpus_declarations.hpp"
        "tests/presentation/animation/corpus_declarations_land.hpp"
        "tests/presentation/animation/corpus_declarations_other.hpp"
        "tests/presentation/animation/corpus_declarations_space.hpp"
        "tests/presentation/animation/corpus_associations.hpp"
        "tests/presentation/animation/corpus_associations_baseline.hpp"
        "tests/presentation/animation/corpus_associations_candidates.hpp"
        "tests/presentation/animation/corpus_diagnostics.hpp"
        "tests/presentation/animation/corpus_association_frozen.tsv"
        "include/eawr/data/xml.hpp"
        "src/data/xml.cpp"
        "src/data/xml_merge.cpp"
        "src/data/xml_registry.cpp"
        "plan/inventories/xml-tags.json"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_STRUCTURAL_DECLARATION_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_STRUCTURAL_DECLARATION_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_STRUCTURAL_DECLARATION_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_STRUCTURAL_DECLARATION_TOOL_SOURCES EAWR_STRUCTURAL_DECLARATION_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/structural_declaration_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_structural_declaration_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_STRUCTURAL_DECLARATION_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_STRUCTURAL_DECLARATION_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_corpus_structural_declaration PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
endif()

if(TARGET animation_deformation_probe)
    # Content identity of the sources that define the deformation receipt.
    set(EAWR_DEFORMATION_TOOL_SOURCES
        "tests/presentation/animation/deformation_probe.cpp"
        "tests/presentation/animation/deformation_probe.hpp"
        "src/presentation/animation/animation.cpp"
        "include/eawr/presentation/animation/animation.hpp"
        "src/assets/model.cpp"
        "src/assets/animation.cpp"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_DEFORMATION_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_DEFORMATION_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_DEFORMATION_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_DEFORMATION_TOOL_SOURCES EAWR_DEFORMATION_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/deformation_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_deformation_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_DEFORMATION_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_DEFORMATION_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_deformation_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
endif()

if(TARGET animation_corpus_source_versions)
    target_compile_definitions(animation_corpus_source_versions_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv")
    # Content identity of the sources that define the source-version receipt.
    set(EAWR_SOURCE_VERSIONS_TOOL_SOURCES
        "tests/presentation/animation/corpus_source_versions.cpp"
        "tests/presentation/animation/corpus_source_versions.hpp"
        "tests/presentation/animation/corpus_associations.hpp"
        "tests/presentation/animation/corpus_associations_baseline.hpp"
        "tests/presentation/animation/corpus_associations_candidates.hpp"
        "tests/presentation/animation/corpus_diagnostics.hpp"
        "tests/presentation/animation/corpus_association_frozen.tsv"
        "src/presentation/animation/animation.cpp"
        "src/assets/model.cpp"
        "src/assets/animation.cpp"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_SOURCE_VERSIONS_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_SOURCE_VERSIONS_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_SOURCE_VERSIONS_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_SOURCE_VERSIONS_TOOL_SOURCES EAWR_SOURCE_VERSIONS_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/source_versions_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_source_versions_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_SOURCE_VERSIONS_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_SOURCE_VERSIONS_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_corpus_source_versions PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
endif()

if(TARGET animation_corpus_structural_discovery)
    target_compile_definitions(animation_corpus_structural_discovery_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv")
    # Content identity of the sources that define the structural-discovery receipt.
    set(EAWR_STRUCTURAL_DISCOVERY_TOOL_SOURCES
        "tests/presentation/animation/corpus_structural_discovery.cpp"
        "tests/presentation/animation/corpus_structural_discovery.hpp"
        "tests/presentation/animation/corpus_source_versions.hpp"
        "tests/presentation/animation/corpus_associations.hpp"
        "tests/presentation/animation/corpus_associations_baseline.hpp"
        "tests/presentation/animation/corpus_associations_candidates.hpp"
        "tests/presentation/animation/corpus_diagnostics.hpp"
        "tests/presentation/animation/corpus_association_frozen.tsv"
        "src/presentation/animation/animation.cpp"
        "src/assets/model.cpp"
        "src/assets/animation.cpp"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_STRUCTURAL_DISCOVERY_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_STRUCTURAL_DISCOVERY_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_STRUCTURAL_DISCOVERY_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_STRUCTURAL_DISCOVERY_TOOL_SOURCES EAWR_STRUCTURAL_DISCOVERY_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/structural_discovery_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_structural_discovery_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_STRUCTURAL_DISCOVERY_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_STRUCTURAL_DISCOVERY_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_corpus_structural_discovery PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
endif()

if(TARGET animation_corpus_model_match_discovery)
    target_compile_definitions(animation_corpus_model_match_discovery_tests PRIVATE
        EAWR_ASSOCIATION_FROZEN_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv")
    # Content identity of the sources that define the model-match discovery receipt.
    set(EAWR_MODEL_MATCH_DISCOVERY_TOOL_SOURCES
        "tests/presentation/animation/corpus_model_match_discovery.cpp"
        "tests/presentation/animation/corpus_model_match_discovery.hpp"
        "tests/presentation/animation/corpus_structural_discovery.hpp"
        "tests/presentation/animation/corpus_source_versions.hpp"
        "tests/presentation/animation/corpus_associations.hpp"
        "tests/presentation/animation/corpus_associations_baseline.hpp"
        "tests/presentation/animation/corpus_associations_candidates.hpp"
        "tests/presentation/animation/corpus_diagnostics.hpp"
        "tests/presentation/animation/corpus_association_frozen.tsv"
        "src/presentation/animation/animation.cpp"
        "src/assets/model.cpp"
        "src/assets/animation.cpp"
        "src/vfs/vfs.cpp"
        "src/vfs/vfs_archive.cpp")
    set(EAWR_MODEL_MATCH_DISCOVERY_TOOL_HASHES "")
    foreach(source IN LISTS EAWR_MODEL_MATCH_DISCOVERY_TOOL_SOURCES)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_ROOT}/${source}")
        file(SHA256 "${EAWR_ROOT}/${source}" source_hash)
        string(APPEND EAWR_MODEL_MATCH_DISCOVERY_TOOL_HASHES "    {\"${source}\", \"${source_hash}\"},
")
    endforeach()
    list(LENGTH EAWR_MODEL_MATCH_DISCOVERY_TOOL_SOURCES EAWR_MODEL_MATCH_DISCOVERY_TOOL_SOURCE_COUNT)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/identity/model_match_discovery_tool_identity.hpp" CONTENT
"#pragma once
#include <array>
#include <string_view>
#include <utility>
namespace eawr_model_match_discovery_tool {
inline constexpr std::array<std::pair<std::string_view, std::string_view>, @EAWR_MODEL_MATCH_DISCOVERY_TOOL_SOURCE_COUNT@> source_hashes{{
@EAWR_MODEL_MATCH_DISCOVERY_TOOL_HASHES@}};
}
" @ONLY)
    target_include_directories(animation_corpus_model_match_discovery PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/identity")
endif()

enable_testing()
add_test(NAME animation_cpu_contracts COMMAND animation_cpu_tests)
add_test(NAME animation_idle_playback_contracts COMMAND animation_idle_playback_tests)
add_test(NAME animation_unit_clips_contracts COMMAND animation_unit_clips_tests)
add_test(NAME animation_corpus_diagnostics COMMAND animation_corpus_diagnostics_tests)
add_test(NAME animation_corpus_associations COMMAND animation_corpus_associations_tests)
if(TARGET animation_corpus_validation)
    add_test(NAME animation_corpus_frozen_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_validation>"
        "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/frozen-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_frozen_cli_test.cmake")
endif()
if(TARGET animation_corpus_declarations)
    add_test(NAME animation_corpus_declarations COMMAND animation_corpus_declarations_tests)
    add_test(NAME animation_corpus_declarations_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_declarations>"
        "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DPAIRS=${CMAKE_CURRENT_SOURCE_DIR}/corpus_declaration_pairs.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/declarations-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_declarations_cli_test.cmake")
endif()
if(TARGET animation_corpus_structural_declaration)
    add_test(NAME animation_corpus_structural_declaration COMMAND animation_corpus_structural_declaration_tests)
    add_test(NAME animation_corpus_structural_declaration_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_structural_declaration>"
        "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/structural-declaration-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_structural_declaration_cli_test.cmake")
endif()
add_test(NAME animation_deformation_probe_controls COMMAND animation_deformation_probe_tests)
if(TARGET animation_deformation_probe)
    add_test(NAME animation_deformation_probe_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_deformation_probe>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/deformation-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/deformation_probe_cli_test.cmake")
endif()
add_test(NAME animation_shot_readiness_controls COMMAND animation_shot_readiness_tests)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME animation_corpus_association_foc COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_foc.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_foc.tsv"
    "${EAWR_ROOT}/include/eawr/presentation/animation/unit_clips.hpp")
add_test(NAME animation_corpus_association_foc_tamper COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_foc_tamper.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
    "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_foc.tsv"
    "${EAWR_ROOT}/include/eawr/presentation/animation/unit_clips.hpp")
if(TARGET animation_corpus_association_foc)
    # Opt-in: skips unless EAWR_EAW_GAME_ROOT is set.
    add_test(NAME animation_corpus_association_foc_rederive COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_association_foc>"
        "-DFROZEN=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DCOMMITTED=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_foc.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/association-foc-rederive-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_foc_rederive.cmake")
    set_tests_properties(animation_corpus_association_foc_rederive PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
endif()
add_test(NAME animation_shot_readiness_probe_cli COMMAND "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:animation_shot_readiness_probe>"
    "-DPYTHON=${Python3_EXECUTABLE}" "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/shot_readiness_fixture.py"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/shot-readiness-cli-$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/shot_readiness_probe_cli_test.cmake")
# Opt-in: skips unless EAWR_EAW_GAME_ROOT and EAWR_REMAKE_MOD_ROOT are set.
add_test(NAME animation_real_unit_samples COMMAND "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:animation_shot_readiness_probe>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/real-unit-samples-$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/shot_readiness_real_samples.cmake")
set_tests_properties(animation_real_unit_samples PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
if(TARGET animation_corpus_source_versions)
    add_test(NAME animation_corpus_source_versions COMMAND animation_corpus_source_versions_tests)
    add_test(NAME animation_corpus_source_versions_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_source_versions>"
        "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/source-versions-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_source_versions_cli_test.cmake")
endif()
if(TARGET animation_corpus_structural_discovery)
    add_test(NAME animation_corpus_structural_discovery COMMAND animation_corpus_structural_discovery_tests)
    add_test(NAME animation_corpus_structural_discovery_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_structural_discovery>"
        "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/structural-discovery-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_structural_discovery_cli_test.cmake")
endif()
if(TARGET animation_corpus_model_match_discovery)
    add_test(NAME animation_corpus_model_match_discovery COMMAND animation_corpus_model_match_discovery_tests)
    add_test(NAME animation_corpus_model_match_discovery_cli COMMAND "${CMAKE_COMMAND}"
        "-DEXE=$<TARGET_FILE:animation_corpus_model_match_discovery>"
        "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/corpus_association_frozen.tsv"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/model-match-discovery-cli-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/corpus_model_match_discovery_cli_test.cmake")
endif()

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
