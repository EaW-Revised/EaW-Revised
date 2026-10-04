# Shared translation units for the root libraries and the viewer.
# Paths are absolute so both projects can include this list directly.
# ROOT_ONLY lists preserve the root build's existing additional units.

set(EAWR_PRESENTATION_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/animation/animation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/animation/idle_playback.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/animation/unit_clips.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/audio/sfx.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/camera.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/constants_source.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/controller.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/free_camera.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/input.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/camera/overview.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/fog/fog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lighting/lighting.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lighting/scene_bloom.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/lighting/wind.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/godot/renderer_contract.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/alo_particles.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/map_effect_plan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/cpu_system.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/proxy_binding.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/render.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/render_plan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/render_stream.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/particles/effect_registry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/debris.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/environment_scene.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/fog_field.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/live_units.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/projectiles.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/shield_hits.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/space.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/space_effects.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/space_geometry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/space_surfaces.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/sun_retail.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/space/unit_fade.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/terrain/terrain.cpp"
)

set(EAWR_PRESENTATION_ROOT_ONLY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/animation/shot_readiness.cpp"
)
