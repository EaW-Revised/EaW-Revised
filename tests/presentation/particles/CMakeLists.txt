if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(eawr_particle_cpu_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_executable(
        particle_cpu_tests
        main.cpp
        "${EAWR_ROOT}/src/presentation/particles/alo_particles.cpp"
        "${EAWR_ROOT}/src/presentation/particles/cpu_system.cpp"
    )
    add_executable(
        particle_render_tests
        render_main.cpp
        render_batch_tests.cpp
        render_proxy_tests.cpp
        render_stream_tests.cpp
        render_submission_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
        "${EAWR_ROOT}/src/presentation/particles/alo_particles.cpp"
        "${EAWR_ROOT}/src/presentation/particles/cpu_system.cpp"
        "${EAWR_ROOT}/src/presentation/particles/proxy_binding.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render_plan.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render_stream.cpp"
        "${EAWR_ROOT}/src/presentation/particles/effect_registry.cpp"
    )
    add_executable(
        map_attachment_owner_tests
        map_attachment_owner_main.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
        "${EAWR_ROOT}/src/presentation/particles/alo_particles.cpp"
        "${EAWR_ROOT}/src/presentation/particles/cpu_system.cpp"
        "${EAWR_ROOT}/src/presentation/particles/map_effect_plan.cpp"
        "${EAWR_ROOT}/src/presentation/particles/proxy_binding.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render_plan.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render_stream.cpp"
        "${EAWR_ROOT}/src/presentation/particles/effect_registry.cpp"
        "${EAWR_ROOT}/src/sim/math/fixed.cpp"
        "${EAWR_ROOT}/src/sim/math/geometry.cpp"
        "${EAWR_ROOT}/src/sim/math/wide.cpp"
    )
    add_executable(
        emitter_metadata_tests
        emitter_metadata_main.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/animation/animation.cpp"
        "${EAWR_ROOT}/src/presentation/particles/alo_particles.cpp"
        "${EAWR_ROOT}/src/presentation/particles/cpu_system.cpp"
        "${EAWR_ROOT}/src/presentation/particles/map_effect_plan.cpp"
        "${EAWR_ROOT}/src/presentation/particles/proxy_binding.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render_plan.cpp"
        "${EAWR_ROOT}/src/presentation/particles/render_stream.cpp"
        "${EAWR_ROOT}/src/presentation/particles/effect_registry.cpp"
        "${EAWR_ROOT}/src/sim/math/fixed.cpp"
        "${EAWR_ROOT}/src/sim/math/geometry.cpp"
        "${EAWR_ROOT}/src/sim/math/wide.cpp"
    )
    foreach(target particle_cpu_tests particle_render_tests map_attachment_owner_tests emitter_metadata_tests)
        target_include_directories(${target} PRIVATE "${EAWR_ROOT}/include")
        # L1.2: the latch, reuse-cache and frame-counter headers of the Godot backend are engine-free.
        target_include_directories(${target} PRIVATE "${EAWR_ROOT}/src/presentation/godot")
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endforeach()
    # The batch contracts (#638) run their tasks on std::thread.
    find_package(Threads REQUIRED)
    target_link_libraries(particle_render_tests PRIVATE Threads::Threads)
else()
    add_executable(particle_cpu_tests main.cpp)
    add_executable(particle_render_tests
        render_main.cpp
        render_batch_tests.cpp
        render_lane_tests.cpp
        render_proxy_tests.cpp
        render_stream_tests.cpp
        render_submission_tests.cpp
        # L1.3: the viewer's engine-free lane parts (the parked thread, its guard window and the exit-code contracts).
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/particle_lane.cpp"
        "${PROJECT_SOURCE_DIR}/apps/viewer/src/particle_workers.cpp")
    add_executable(map_attachment_owner_tests map_attachment_owner_main.cpp)
    add_executable(emitter_metadata_tests emitter_metadata_main.cpp)
    foreach(target particle_cpu_tests particle_render_tests map_attachment_owner_tests emitter_metadata_tests)
        target_link_libraries(${target} PRIVATE eawr::presentation)
        target_compile_features(${target} PRIVATE cxx_std_20)
        eawr_target_project_warnings(${target})
    endforeach()
    # The batch contracts (#638) run their tasks on std::thread.
    find_package(Threads REQUIRED)
    target_link_libraries(particle_render_tests PRIVATE Threads::Threads)
    # L1.2: the latch, reuse-cache and frame-counter headers of the Godot backend are engine-free.
    target_include_directories(particle_render_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/presentation/godot")
    target_include_directories(particle_render_tests PRIVATE "${PROJECT_SOURCE_DIR}/apps/viewer/src")
    target_link_libraries(particle_render_tests PRIVATE eawr::platform)
    target_compile_definitions(particle_render_tests PRIVATE EAWR_LANE_CONTRACTS=1)
endif()

enable_testing()
add_test(NAME particle_cpu_contracts COMMAND particle_cpu_tests)
add_test(NAME particle_render_contracts COMMAND particle_render_tests)
add_test(NAME map_attachment_owner_contracts COMMAND map_attachment_owner_tests)
add_test(NAME emitter_metadata_contracts COMMAND emitter_metadata_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
