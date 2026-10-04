if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(eawr_camera_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_executable(camera_tests
        main.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/free_camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/input.cpp"
    )
    target_include_directories(camera_tests PRIVATE "${EAWR_ROOT}/include")
    add_executable(free_camera_tests
        free_camera_main.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/free_camera.cpp"
    )
    target_include_directories(free_camera_tests PRIVATE "${EAWR_ROOT}/include")
    # The viewer's engine-independent binding loader/adapter (no Godot).
    add_executable(camera_bindings_tests
        bindings_json_tests.cpp
        bindings_main.cpp
        camera_binding_event_tests.cpp
        "${EAWR_ROOT}/apps/viewer/src/camera_binding_config.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_tokens.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_values.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_table.cpp"
        "${EAWR_ROOT}/apps/viewer/src/camera_input.cpp"
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/free_camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/input.cpp"
    )
    target_include_directories(camera_bindings_tests PRIVATE
        "${EAWR_ROOT}/include" "${EAWR_ROOT}/apps/viewer/src")
    add_executable(camera_constants_source_tests
        constants_source_main.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/constants_source.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
    )
    target_include_directories(camera_constants_source_tests PRIVATE
        "${EAWR_ROOT}/include" "${EAWR_ROOT}/third_party/pugixml/src")
    if(MSVC)
        target_compile_options(camera_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(free_camera_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(camera_bindings_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(camera_constants_source_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(camera_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(free_camera_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(camera_bindings_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(camera_constants_source_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    set(EAWR_ROOT "${PROJECT_SOURCE_DIR}")
    add_executable(camera_tests main.cpp)
    target_link_libraries(camera_tests PRIVATE eawr::presentation)
    target_compile_features(camera_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(camera_tests)
    add_executable(free_camera_tests free_camera_main.cpp)
    target_link_libraries(free_camera_tests PRIVATE eawr::presentation)
    target_compile_features(free_camera_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(free_camera_tests)
    add_executable(camera_bindings_tests
        bindings_json_tests.cpp
        bindings_main.cpp
        camera_binding_event_tests.cpp
        "${EAWR_ROOT}/apps/viewer/src/camera_binding_config.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_tokens.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_values.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_table.cpp"
        "${EAWR_ROOT}/apps/viewer/src/camera_input.cpp"
    )
    target_include_directories(camera_bindings_tests PRIVATE "${EAWR_ROOT}/apps/viewer/src")
    target_link_libraries(camera_bindings_tests PRIVATE eawr::presentation)
    target_compile_features(camera_bindings_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(camera_bindings_tests)
    add_executable(camera_constants_source_tests constants_source_main.cpp)
    target_link_libraries(camera_constants_source_tests PRIVATE eawr::presentation)
    target_compile_features(camera_constants_source_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(camera_constants_source_tests)
endif()
add_executable(camera_controller_tests controller_main.cpp)
target_compile_features(camera_controller_tests PRIVATE cxx_std_20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_sources(camera_controller_tests PRIVATE
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/controller.cpp"
    )
    target_include_directories(camera_controller_tests PRIVATE "${EAWR_ROOT}/include")
    if(MSVC)
        target_compile_options(camera_controller_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(camera_controller_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    target_link_libraries(camera_controller_tests PRIVATE eawr::presentation)
    eawr_target_project_warnings(camera_controller_tests)
endif()
target_compile_definitions(camera_bindings_tests PRIVATE
    EAWR_CAMERA_BINDINGS_PATH="${EAWR_ROOT}/apps/viewer/project/config/camera-bindings.json")

# The viewer's engine-independent map camera bridge (land and space context).
add_executable(map_camera_bridge_tests
    map_camera_bridge_main.cpp
    map_camera_config_tests.cpp
    map_camera_lifecycle_tests.cpp
    map_camera_movement_tests.cpp
    map_camera_pan_tests.cpp
    map_camera_terrain_tests.cpp
    map_camera_orbit_tests.cpp
    "${EAWR_ROOT}/apps/viewer/src/map_camera.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_config.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_tokens.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_values.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_binding_table.cpp"
    "${EAWR_ROOT}/apps/viewer/src/camera_input.cpp"
)
target_include_directories(map_camera_bridge_tests PRIVATE "${EAWR_ROOT}/apps/viewer/src")
target_compile_features(map_camera_bridge_tests PRIVATE cxx_std_20)
target_compile_definitions(map_camera_bridge_tests PRIVATE
    EAWR_VIEWER_CONFIG_DIR="${EAWR_ROOT}/apps/viewer/project/config")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    target_sources(map_camera_bridge_tests PRIVATE
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/camera/camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/constants_source.cpp"
        "${EAWR_ROOT}/src/presentation/camera/controller.cpp"
        "${EAWR_ROOT}/src/presentation/camera/free_camera.cpp"
        "${EAWR_ROOT}/src/presentation/camera/input.cpp"
        "${EAWR_ROOT}/third_party/pugixml/src/pugixml.cpp"
    )
    target_include_directories(map_camera_bridge_tests PRIVATE
        "${EAWR_ROOT}/include" "${EAWR_ROOT}/third_party/pugixml/src")
    if(MSVC)
        target_compile_options(map_camera_bridge_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(map_camera_bridge_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    target_link_libraries(map_camera_bridge_tests PRIVATE eawr::presentation pugixml::pugixml)
    eawr_target_project_warnings(map_camera_bridge_tests)
endif()

enable_testing()
add_test(NAME camera_contracts COMMAND camera_tests)
add_test(NAME free_camera_contracts COMMAND free_camera_tests)
add_test(NAME camera_bindings_contracts COMMAND camera_bindings_tests)
add_test(NAME camera_constants_source_contracts COMMAND camera_constants_source_tests)
add_test(NAME camera_controller_contracts COMMAND camera_controller_tests)
add_test(NAME map_camera_bridge_contracts COMMAND map_camera_bridge_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
