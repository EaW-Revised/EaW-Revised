"""Synthetic stand-in for an exported viewer package, driven by EAWR_FAKE_VIEWER_MODE.

It is launched by the package runtime harness tests in place of a real Godot
package and mimics only the observable surface the harness verifies: engine
output, exit code, report JSON and capture PNG. It is never evidence of a
package run.
"""

import hashlib
import json
import os
import struct
import sys
import time
import zlib
from pathlib import Path
from typing import Optional

DEVICE = ("OpenGL API 3.3.0 Core Profile Context 1.2.3 - Compatibility - Using Device: "
          "Synthetic Vendor - Synthetic Adapter")
# Per requested driver: device banner, report rendering_method and driver_api,
# in the forms Godot prints them (RenderingDevice for vulkan and d3d12).
BACKENDS = {
    "opengl3": (DEVICE, "gl_compatibility", "3.3.0 Core Profile Context 1.2.3"),
    "vulkan": ("Vulkan 1.4.318 - Forward+ - Using Device #0: Synthetic Vendor - Synthetic Adapter",
               "forward_plus", "1.4.318"),
    "d3d12": ("D3D12 12_0 - Forward+ - Using Device #0: Synthetic Vendor - Synthetic Adapter",
              "forward_plus", "12_0"),
}
SHADER_ERRORS = (
    "SHADER ERROR: Unknown identifier in expression: 'unknown_runtime_identifier'.\n"
    "   at: (null) (:3)\n"
    "ERROR: Shader compilation failed.\n"
    "   at: set_code (servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp:185)\n"
)

SCENE_IDENTITY = b"eawr-renderer-overlap-v2:opaque,alpha-tested,transparent,post:lifecycle-v1"
REJECTION_MARKER = "EAWR runtime: unknown material route and pass rejected; registry unchanged"
PASSED_MARKER = "EAWR runtime: all exercise checks passed; persisting report"
VSYNC_WARNING = ("WARNING: Could not set V-Sync mode, as changing V-Sync mode is not supported "
                 "by the graphics driver.")
LEAK_WARNING = "WARNING: ObjectDB instances leaked at exit (run with --verbose for details)."


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))


def png(width: int = 1280, height: int = 720, color_type: int = 6,
        idat: Optional[bytes] = None) -> bytes:
    """Build an 8-bit PNG with valid CRCs; ``idat`` replaces the compressed image data."""
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    rows = b"".join(b"\x00" + b"\x00" * (width * channels) for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows) if idat is None else idat) + chunk(b"IEND", b""))


def report(capture_sha256: str, driver: str = "vulkan") -> dict:
    _, method, api = BACKENDS[driver]
    return {
        "schema_version": 1,
        "status": "renderer_runtime_exercise_passed",
        "failure": "",
        "backend": {"engine": "Godot 4.7.2-stable", "rendering_method": method,
                    "adapter_vendor": "Synthetic Vendor", "adapter_name": "Synthetic Adapter",
                    "driver_api": api},
        "versions": {"godot": "4.7.2-stable", "godot_cpp": "10.0.0-stable", "material_schema": 1},
        "scene_sha256": hashlib.sha256(SCENE_IDENTITY).hexdigest(),
        "replay": {"logical_path": "res://common/original-v1.eawr-replay", "sha256": ""},
        "snapshot_count": 1,
        "capture_sha256": capture_sha256,
        "capture_identity": {"vfs_profile": "remake", "viewport": {"width": 1280, "height": 720}},
        "model": {"logical_path": "", "sha256": ""},
        "texture": {"logical_path": "", "sha256": ""},
        "material_program": "",
        "animation": {"logical_path": "", "sha256": "", "bone_count": 0},
        "animation_time_seconds": 0,
        "skin_palette_bound": False,
        "animation_capture_verified": False,
        "renderer_runtime_exercise": True,
        "runtime_capture_verified": True,
        "runtime_overlap_verified": True,
        "runtime_post_dependency_verified": True,
        "runtime_failed_upload_registry_clean": True,
        "runtime_scene_switch_count": 48,
        "runtime_shutdown_resources_empty": True,
        "tactical_camera_verified": False,
        "atlas_overlay_verified": False,
        "pass_order": ["opaque", "alpha-tested", "transparent", "post"],
    }


def option(argv: list, name: str):
    return argv[argv.index(name) + 1] if name in argv else None


def main() -> int:
    mode = os.environ.get("EAWR_FAKE_VIEWER_MODE", "pass")
    argv = sys.argv[1:]
    executable = Path(argv[0])
    # The harness must launch from the package directory with an explicit backend.
    if Path.cwd().resolve() != executable.parent.resolve():
        print("fake viewer: not launched from the package directory", file=sys.stderr)
        return 3
    driver = option(argv, "--rendering-driver")
    if driver not in BACKENDS or option(argv, "--display-driver") is None:
        print("fake viewer: backend was not explicit", file=sys.stderr)
        return 3
    report_path = Path(option(argv, "--eawr-report"))
    capture_arg = option(argv, "--eawr-capture")

    identity = os.environ.get("EAWR_FAKE_ENGINE_IDENTITY", "4.7.2.stable.official.ed1daf0bf")
    stamp = " (2026-08-17 01:18:38 UTC)" if ".custom_build." in identity else ""
    banner = f"Godot Engine v{identity}{stamp} - https://godotengine.org"
    print(banner if mode != "wrong_engine" else banner.replace("4.7.2", "4.7.1"))
    # gl_fallback: Godot could not start the requested RenderingDevice driver
    # and fell back to OpenGL, as it does by default.
    if mode == "gl_fallback":
        driver = "opengl3"
    if mode != "no_device_banner":
        print(BACKENDS[driver][0])
    sys.stdout.flush()
    if mode != "no_shader_error":
        sys.stderr.write(SHADER_ERRORS)
    if mode == "vsync_warning":
        sys.stderr.write(VSYNC_WARNING + "\n     at: set_use_vsync (gl_manager_x11.cpp:372)\n")
    if mode == "unexpected_error":
        sys.stderr.write("ERROR: Condition \"!rid\" is true.\n")
    if mode == "leak":
        sys.stderr.write("WARNING: 3 RID allocations of type 'Material' were leaked at exit.\n")
    if mode == "objectdb_leak":
        sys.stderr.write(LEAK_WARNING + "\n     at: cleanup (core/object/object.cpp:2481)\n")
    if mode == "timeout":
        time.sleep(60)
    if mode == "early_failure":
        # A failing check also reports "failed" and exits 2 without reaching
        # the passing path; the persistence probe must not accept this.
        sys.stderr.write("EAWR viewer report output could not be opened\n")
        return 2
    if mode != "no_rejection_marker":
        print(REJECTION_MARKER)
    print(PASSED_MARKER)
    sys.stdout.flush()

    if report_path.is_dir():
        # Report persistence failure: a fixed viewer exits 2, a regressed one 0.
        sys.stderr.write("EAWR viewer report output could not be opened\n")
        return 0 if mode == "swallow_report_failure" else 2

    image = png() if mode != "wrong_size" else png(640, 360)
    if mode == "corrupt_png":
        image = image[:40] + bytes([image[40] ^ 0xFF]) + image[41:]
    if mode == "undecodable_png":
        # Every chunk CRC is valid, but the image data is not a zlib stream.
        image = png(idat=b"not zlib data")
    if capture_arg:
        Path(capture_arg).write_bytes(image)
    body = report(hashlib.sha256(image).hexdigest(), driver)
    if mode == "false_lifecycle":
        body["runtime_failed_upload_registry_clean"] = False
    if mode == "few_switches":
        body["runtime_scene_switch_count"] = 39
    if mode == "capture_mismatch":
        body["capture_sha256"] = hashlib.sha256(b"other").hexdigest()
    if mode == "unhashable_backend":
        body["backend"]["rendering_method"] = []
    if mode == "wrong_adapter":
        body["backend"]["adapter_name"] = "Some Other Adapter"
    if mode == "wrong_scene":
        body["scene_sha256"] = hashlib.sha256(b"another scene").hexdigest()
    if mode == "extra_key":
        body["runtime_invented_field"] = True
    if mode == "render_profile":
        body["render_profile"] = {"name": "retail", "msaa": 0, "smaa": False, "anisotropy": 0}
    if mode == "mutate_package":
        with (executable.parent / (executable.stem + ".pck")).open("ab") as pck:
            pck.write(b"late")
    if mode == "missing_report":
        return 0
    if mode == "malformed_report":
        report_path.write_text('{"status": "renderer_runtime_exercise_passed",', encoding="utf-8")
        return 0
    report_path.write_text(json.dumps(body, indent=2), encoding="utf-8")
    if mode == "stale_report":
        past = time.time() - 3600
        os.utime(report_path, (past, past))
    return 1 if mode == "exit_nonzero" else 0


if __name__ == "__main__":
    raise SystemExit(main())
