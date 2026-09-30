add_library(
    eawr_presentation STATIC
    animation/animation.cpp
    animation/idle_playback.cpp
    animation/shot_readiness.cpp
    animation/unit_clips.cpp
    audio/sfx.cpp
    camera/camera.cpp
    camera/constants_source.cpp
    camera/controller.cpp
    camera/free_camera.cpp
    camera/input.cpp
    camera/overview.cpp
    fog/fog.cpp
    lighting/lighting.cpp
    lighting/scene_bloom.cpp
    lighting/wind.cpp
    godot/renderer_contract.cpp
    particles/alo_particles.cpp
    particles/map_effect_plan.cpp
    particles/cpu_system.cpp
    particles/proxy_binding.cpp
    particles/render.cpp
    space/debris.cpp
    space/environment_scene.cpp
    space/fog_field.cpp
    space/live_units.cpp
    space/projectiles.cpp
    space/shield_hits.cpp
    space/space.cpp
    space/space_effects.cpp
    space/space_geometry.cpp
    space/space_surfaces.cpp
    space/sun_retail.cpp
    space/unit_fade.cpp
    terrain/terrain.cpp
)
add_library(eawr::presentation ALIAS eawr_presentation)
target_include_directories(
    eawr_presentation
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_presentation PUBLIC eawr::core eawr::data eawr::sim PRIVATE pugixml::pugixml)
target_compile_features(eawr_presentation PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_presentation)
