"""Explicit physical members for positional viewer-host source assertions."""
from pathlib import Path

SRC = Path(__file__).resolve().parents[3] / "apps/viewer/src"

HOST_FUNCTION_MEMBERS = {
    "bool ViewerHost::start_camera_interaction(": "viewer_host_camera_setup.cpp",
    "bool ViewerHost::step_camera_interaction(": "viewer_host_camera_steps.cpp",
    "bool ViewerHost::toggle_free_camera(": "viewer_host_camera_steps.cpp",
    "bool ViewerHost::step_free_camera(": "viewer_host_camera_steps.cpp",
    "bool ViewerHost::advance_camera_selftest(": "viewer_host_camera_probe.cpp",
    "void ViewerHost::_input(": "viewer_host_camera_setup.cpp",
    "void ViewerHost::_unhandled_input(": "viewer_host_camera_setup.cpp",
    "void ViewerHost::route_world_input(": "viewer_host_camera_setup.cpp",
    "void ViewerHost::_notification(": "viewer_host_camera_setup.cpp",
    "void ViewerHost::sync_modal_holds(": "viewer_host_camera_setup.cpp",
    "void ViewerHost::_process(": "viewer_host_process.cpp",
    "BoundedRead read_bytes_bounded(": "viewer_host_camera_setup.cpp",
    "std::optional<std::vector<std::byte>> viewer_host_detail::read_bytes(": "viewer_host_scene.cpp",
    "std::string json(const std::string_view value)": "viewer_host_report.cpp",
}


def host_source(signature: str) -> str:
    return (SRC / HOST_FUNCTION_MEMBERS[signature]).read_text(encoding="utf-8")
