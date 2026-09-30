"""Wholly original synthetic tactical camera fixture for the viewer.

Every value here is invented for this test. None is a retail constant, and the
numbers are deliberately unlike the shipped ones so a fixture value can never be
mistaken for original-game evidence. The real constants live only in
``plan/inventories/camera-constants.json``, which is generated from the
installed corpus and records tag names, values and logical source paths.

The fixture materialises a minimal single-layer VFS data root containing
``data/xml/tacticalcameras.xml`` and ``data/xml/gameconstants.xml``, so the
viewer's camera mode can be exercised end to end without any retail corpus.
"""

from __future__ import annotations

import pathlib

# --- Land_Mode: spline driven, so the viewer exercises the spline path. -------
LAND = {
    "Distance_Min": "100.0",
    "Distance_Max": "500.0",
    "Distance_Default": "300.0",
    "Distance_Per_Mouse_Unit": "80.0",
    "Distance_Smooth_Time": "0.2",
    "Pitch_Min": "20.0",
    "Pitch_Max": "60.0",
    "Pitch_Default": "40.0",
    "Pitch_Per_Mouse_Unit": "0.0",
    "Pitch_Per_Zoom_Unit": "0.0",
    "Pitch_When_Zoomed_In": "20.0",
    "Pitch_Zoom_Begin_Fraction": "-1.0",
    "Yaw_Min": "-1000.0",
    "Yaw_Max": "1000.0",
    "Yaw_Default": "0.0",
    "Yaw_Per_Mouse_Unit": "1.25",
    # #515: FoC reads the angle as horizontal on 4:3 and draws that screen's vertical angle;
    # 63.742 draws the 50 degree vertical view these fixtures were framed with.
    "Fov_Min": "30.0",
    "Fov_Max": "63.742",
    "Fov_Default": "63.742",
    "Fov_Per_Mouse_Unit": "0.0",
    "Near_Clip": "5.0",
    "Far_Clip": "8000.0",
    "Use_Splines": "yes",
    # Mixed ',' and ' ' separators, exactly as Petroglyph authors them.
    "Distance_Spline": "0.0 100.0, 0.25, 200.0, 1.0, 500.0",
    "Pitch_Spline": "0.0,20.0, 0.5,40.0, 1.0, 60.0",
}

# --- Space_Mode: linear distance, constant pitch. -----------------------------
SPACE = {
    "Distance_Min": "400.0",
    "Distance_Max": "1200.0",
    "Distance_Default": "800.0",
    "Distance_Per_Mouse_Unit": "200.0",
    "Distance_Smooth_Time": "0.1",
    "Pitch_Min": "-20.0",
    "Pitch_Max": "80.0",
    "Pitch_Default": "45.0",
    "Pitch_Per_Mouse_Unit": "-1.25",
    "Pitch_Per_Zoom_Unit": "0.0",
    "Pitch_When_Zoomed_In": "45.0",
    "Pitch_Zoom_Begin_Fraction": "-1.0",
    "Yaw_Min": "-1000.0",
    "Yaw_Max": "1000.0",
    "Yaw_Default": "0.0",
    "Yaw_Per_Mouse_Unit": "1.25",
    # #515: 63.742 across 4:3 draws the 50 degree vertical view (see LAND).
    "Fov_Min": "63.742",
    "Fov_Max": "63.742",
    "Fov_Default": "63.742",
    "Fov_Per_Mouse_Unit": "0.0",
    "Near_Clip": "2.0",
    "Far_Clip": "9000.0",
}

SCROLL = {
    "Tactical_Min_Scroll_Speed": "600.0",
    "Tactical_Max_Scroll_Speed": "2400.0",
    "Tactical_Edge_Scroll_Region": "3",
    "Tactical_Offscreen_Scroll_Region": "40",
    "Push_Scroll_Speed_Modifier": "2.5",
    "Scroll_Acceleration_Factor": "0.1",
    "Scroll_Deceleration_Factor": "1.25",
}

MODES = (("Land_Mode", LAND), ("Space_Mode", SPACE))

CAMERA_LOGICAL_PATH = "data/xml/tacticalcameras.xml"
CONSTANTS_LOGICAL_PATH = "data/xml/gameconstants.xml"


def _escape(value: str) -> str:
    return value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def tactical_cameras_xml() -> str:
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<TacticalCameras>"]
    for name, fields in MODES:
        lines.append(f'  <TacticalCamera Name="{name}">')
        for tag, value in fields.items():
            lines.append(f"    <{tag}>{_escape(value)}</{tag}>")
        lines.append("  </TacticalCamera>")
    lines.append("</TacticalCameras>")
    return "\n".join(lines) + "\n"


def game_constants_xml() -> str:
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<Game_Constants>"]
    for tag, value in SCROLL.items():
        lines.append(f"  <{tag}>{_escape(value)}</{tag}>")
    lines.append("</Game_Constants>")
    return "\n".join(lines) + "\n"


def expected_state(mode: str, zoom: float) -> dict[str, float]:
    """The state the pure model must produce, computed from the fixture itself.

    This mirrors the documented derivation rather than the implementation: a
    piecewise-linear spline when Use_Splines is set, a linear distance range
    otherwise, and pan speed interpolated on the normalised distance.
    """
    fields = dict(MODES[0][1] if mode == "land" else MODES[1][1])
    zoom = min(max(zoom, 0.0), 1.0)
    distance_min = float(fields["Distance_Min"])
    distance_max = float(fields["Distance_Max"])

    if fields.get("Use_Splines", "no").lower() in ("yes", "true", "1"):
        distance = _spline(fields["Distance_Spline"], zoom)
        pitch = _spline(fields["Pitch_Spline"], zoom)
    else:
        distance = distance_min + (distance_max - distance_min) * zoom
        pitch = float(fields["Pitch_Default"])
        pitch = min(max(pitch, float(fields["Pitch_Min"])), float(fields["Pitch_Max"]))
    distance = min(max(distance, distance_min), distance_max)

    normalized = (distance - distance_min) / (distance_max - distance_min)
    slow = float(SCROLL["Tactical_Min_Scroll_Speed"])
    fast = float(SCROLL["Tactical_Max_Scroll_Speed"])
    return {
        "zoom": zoom,
        "distance": distance,
        "pitch_degrees": pitch,
        "fov_degrees": float(fields["Fov_Default"]),
        "pan_speed": slow + (fast - slow) * normalized,
    }


def _spline(text: str, fraction: float) -> float:
    tokens = [token for token in text.replace(",", " ").split() if token]
    values = [float(token.rstrip("fF")) for token in tokens]
    points = [(values[i], values[i + 1]) for i in range(0, len(values), 2)]
    if fraction <= points[0][0]:
        return points[0][1]
    if fraction >= points[-1][0]:
        return points[-1][1]
    for earlier, later in zip(points, points[1:]):
        if fraction > later[0]:
            continue
        span = later[0] - earlier[0]
        if span <= 0.0:
            return later[1]
        t = (fraction - earlier[0]) / span
        return earlier[1] + (later[1] - earlier[1]) * t
    return points[-1][1]


def default_zoom(mode: str) -> float:
    """The normalised zoom the viewer uses when --eawr-camera-zoom is absent."""
    fields = MODES[0][1] if mode == "land" else MODES[1][1]
    distance_min = float(fields["Distance_Min"])
    distance_max = float(fields["Distance_Max"])
    default = float(fields["Distance_Default"])
    return min(max((default - distance_min) / (distance_max - distance_min), 0.0), 1.0)


def write_fixture_root(root: pathlib.Path) -> pathlib.Path:
    """Materialise a minimal single-layer VFS data root and return it."""
    data = root / "Data"
    xml_directory = data / "xml"
    xml_directory.mkdir(parents=True, exist_ok=True)
    # The mount contract requires at least one declared archive; this fixture is
    # loose-only, so it declares one that is deliberately absent.
    (data / "MegaFiles.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<Mega_Files>\n  <File>EawrCameraFixture.meg</File>\n</Mega_Files>\n",
        encoding="utf-8",
    )
    (xml_directory / "tacticalcameras.xml").write_text(tactical_cameras_xml(),
                                                       encoding="utf-8")
    (xml_directory / "gameconstants.xml").write_text(game_constants_xml(), encoding="utf-8")
    return root


if __name__ == "__main__":
    import sys

    print(write_fixture_root(pathlib.Path(sys.argv[1])))
