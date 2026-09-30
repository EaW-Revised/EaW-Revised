"""Fixture pins, report verification and image helpers for the MC-50 Hull shadow probe.

Shared by test_hull_asset_shadow_runtime.py, which documents the probe, its
outputs and the opt-in graphical runs.
"""

import hashlib
import importlib.util
from pathlib import Path
import struct
import zlib


ROOT = Path(__file__).resolve().parents[3]
PROJECT = Path(__file__).resolve().parent / "renderer" / "hull_asset_project"
HARNESS_PATH = ROOT / "apps/viewer/tools/qualify_package_runtime.py"
SPEC = importlib.util.spec_from_file_location("qualify_package_runtime", HARNESS_PATH)
assert SPEC is not None and SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)

WIDTH, HEIGHT = 640, 480
NAMES = ("on", "off", "restored_on", "missing_caster", "casting_disabled", "receiving_disabled")
MODEL_SHA256 = "9fd06b06d1626d8a11bea184fe73768745a6774062d44c9c39636e16202b62fe"
TEXTURE_SHA256 = "456e88d85c9569d173c2b68bc4cc59a51e5a201aa2bf4cac8ee23afc26fab7cb"
MIN_MASK_PIXELS = 1500
# Predeclared thresholds (decoded luma levels, Rec. 709 weights).
DARKENING = 6.0
CONTROL = 2.0
# Per-pixel geometric agreement gate: the GPU shadow must land where the CPU
# ray tracer predicts it.
PIXEL_DARKENING = 6.0
PIXEL_STABLE = 3.0
RECEIVER_AGREEMENT = 0.85
CONTROL_AGREEMENT = 0.90
ROUTES = ("rigid_skinned", "unskinned")
# Matched-route gate: #52 gives both uploads the same bind-space vertices.
# The distinct GPU skin paths may still round a few pixels by less than one
# decoded level (Forward+ skins in a compute pass: 10-23 pixels, 3 of them in
# the lit control, on the RX 7900 XTX); receiver-mask pixels must match exactly.
ROUTE_MAX_DIFFERING_PIXELS = 32
ROUTE_MAX_CONTROL_DIFFERING_PIXELS = 4
ROUTE_MAX_CONTROL_LUMA_DIFFERENCE = 1.0
CRITERIA_FAILURES = (
    "Hull receiver did not darken by six decoded levels",
    "lit Hull control changed",
    "receiver contradicts on/off control",
    "lit control changed",
)
# The declared fixture (docs/rendering.md#hull-shadow-fixture), pinned
# independently of any run: the CPU mask, view and camera that were fixed before
# the first capture, and the caster placement and shadow distance they come
# from. A report that differs was made by another fixture or by a diagnostic
# flag and is not evidence for the documented verdicts.
HULL_FIXTURE = {
    # #52 frames rigid Hull bounds after the bone's rest transform. The tiny
    # camera shift changes one edge pixel; the model and selected view remain the same.
    "mask_sha256": "8724a244e8b4cc2ffe8add2734b16712f9db44dedd36dfa36c1d7cf84e4070a8",
    "lit": 6332,
    "shadowed": 4797,
    "selected": 0,
    # (view direction, lit, shadowed) for the five predeclared views.
    "candidates": (((1.0, 0.45, 0.35), 6332, 4797), ((0.6, 1.0, -0.6), 4033, 2272),
                   ((-0.6, 1.0, -0.6), 8768, 751), ((0.0, 1.0, -0.35), 9922, 2355),
                   ((0.8, 0.6, -0.9), 4051, 3328)),
    "eye": (286.963318, 133.729446, 95.241806),
    "target": (0.0, 4.59595966, -5.19535828),
    "up": (0.0, 1.0, 0.0),
    "near": 114.665314,
    "far": 728.63855,
    "caster_sun_distance": 120,
    "caster_lateral_offset": 70,
    # 120 units along toward-light (0, 0.7071068, 0.7071068) plus 70 to render +X.
    "caster_translation": (70.0, 84.8528137, 84.8528137),
    # ceil(1.1 * far_distance / 0.8): the fixture's own value, not a scaled one.
    "max_distance": 615,
    "far_distance": 447.202087,
    # The #52 bind-space Hull surface both upload routes send (positions,
    # normals, tangents, binormals, UV0, indices; skin data excluded).
    "surface_sha256": "74eddbd3a7c42bc4e1b30cbb856de51598e7173395631395859f26769801f694",
    "model_bones": 51,
}
FIXTURE_TOLERANCE = 1e-3
# Declared run configurations: whether the receiver Hull casts shadows, and
# which upload route it takes (the skinning control repeats both casting
# settings on the unskinned route; every other flag stays at its default).
RECEIVER_CASTS = {"noncasting_receiver": False, "self_casting_receiver": True,
                  "noncasting_receiver_unskinned": False, "self_casting_receiver_unskinned": True}
UPLOAD_ROUTES = {label: "unskinned" if label.endswith("_unskinned") else "rigid_skinned" for label in RECEIVER_CASTS}


def read_png(path):
    raw = Path(path).read_bytes()
    if not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("PNG signature")
    offset = 8
    compressed = bytearray()
    dimensions = None
    while offset + 12 <= len(raw):
        length = struct.unpack_from(">I", raw, offset)[0]
        end = offset + 12 + length
        if end > len(raw):
            raise ValueError("truncated PNG chunk")
        kind = raw[offset + 4:offset + 8]
        payload = raw[offset + 8:offset + 8 + length]
        if zlib.crc32(kind + payload) != struct.unpack_from(">I", raw, offset + 8 + length)[0]:
            raise ValueError("PNG chunk CRC")
        if kind == b"IHDR":
            width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
            dimensions = (width, height)
            if depth != 8 or color != 6 or compression or filtering or interlace:
                raise ValueError("expected noninterlaced RGBA8 PNG")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
        offset = end
    if dimensions != (WIDTH, HEIGHT):
        raise ValueError("PNG dimensions")
    scanlines = zlib.decompress(compressed)
    stride = WIDTH * 4
    output = bytearray(HEIGHT * stride)
    position = 0
    prior = bytearray(stride)
    for row in range(HEIGHT):
        if position + 1 + stride > len(scanlines):
            raise ValueError("truncated PNG scanlines")
        filter_type = scanlines[position]
        position += 1
        current = bytearray(scanlines[position:position + stride])
        position += stride
        if filter_type > 4:
            raise ValueError("unsupported PNG filter")
        if filter_type == 2:
            current = bytearray((a + b) & 255 for a, b in zip(current, prior))
        elif filter_type:
            for column in range(stride):
                left = current[column - 4] if column >= 4 else 0
                above = prior[column]
                upper_left = prior[column - 4] if column >= 4 else 0
                if filter_type == 1:
                    predictor = left
                elif filter_type == 3:
                    predictor = (left + above) // 2
                else:
                    base = left + above - upper_left
                    distances = (abs(base - left), abs(base - above), abs(base - upper_left))
                    predictor = (left, above, upper_left)[distances.index(min(distances))]
                current[column] = (current[column] + predictor) & 255
        output[row * stride:(row + 1) * stride] = current
        prior = current
    if position != len(scanlines):
        raise ValueError("extra PNG scanlines")
    return output


def write_png(path, pixels):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    stride = WIDTH * 4
    scanlines = b"".join(b"\x00" + bytes(pixels[row * stride:(row + 1) * stride]) for row in range(HEIGHT))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def read_pgm(path):
    raw = Path(path).read_bytes()
    header = f"P5\n{WIDTH} {HEIGHT}\n2\n".encode()
    if not raw.startswith(header) or len(raw) != len(header) + WIDTH * HEIGHT:
        raise ValueError("mask PGM header or size")
    classes = raw[len(header):]
    if any(value > 2 for value in classes):
        raise ValueError("mask PGM class out of range")
    return classes


def write_pgm(path, classes):
    Path(path).write_bytes(f"P5\n{WIDTH} {HEIGHT}\n2\n".encode() + bytes(classes))


def luma_at(pixels, index):
    offset = index * 4
    return 0.2126 * pixels[offset] + 0.7152 * pixels[offset + 1] + 0.0722 * pixels[offset + 2]


def luma(sample):
    return sum(a * b for a, b in zip(sample["rgb"], (0.2126, 0.7152, 0.0722)))


_MASK_INDICES = {}


def mask_indices(classes, wanted):
    """Pixel indices of one mask class, in order; the verifier asks for the same few masks many times."""
    key = (bytes(classes), wanted)
    if key not in _MASK_INDICES:
        _MASK_INDICES[key] = tuple(index for index, value in enumerate(classes) if value == wanted)
    return _MASK_INDICES[key]


def mask_mean(pixels, classes, wanted):
    indices = mask_indices(classes, wanted)
    count = len(indices)
    totals = [sum(pixels[index * 4 + channel] for index in indices) for channel in range(3)]
    return {"pixels": count, "rgb": [value / count for value in totals] if count else [0, 0, 0]}


def close(values, expected, tolerance=FIXTURE_TOLERANCE):
    return (isinstance(values, list) and len(values) == len(expected)
            and all(isinstance(v, (int, float)) and abs(v - e) <= tolerance for v, e in zip(values, expected)))


def verify(report, directory, label, fixture=HULL_FIXTURE):
    """Return (integrity errors, verdict). Pixels and the mask file are the authority.

    label names the declared run configuration (a RECEIVER_CASTS key); fixture
    holds the independently pinned mask, view, camera, placement and uploaded
    surface."""
    errors = []

    def require(condition, reason):
        if not condition:
            errors.append(reason)

    require(report.get("schema") == "eawr-hull-asset-shadow-probe-v1", "schema")
    require(report.get("source") == "alo_viewer_default" and report.get("policy") == "sh", "SH source/policy")
    asset = report.get("asset", {})
    require(asset.get("model") == "data/art/models/rebel_mon_calamari_mc_50.alo"
            and asset.get("model_sha256") == MODEL_SHA256, "pinned MC-50 model")
    require(asset.get("texture", "").lower() == "data/art/textures/rebel_mon_calamari_tide.dds"
            and asset.get("texture_sha256") == TEXTURE_SHA256, "pinned Hull BaseTexture")
    require(asset.get("mesh") == "Hull" and asset.get("vertices") == 20084 and asset.get("triangles") == 13094,
            "Hull mesh")
    require(report.get("material", {}).get("program") == "MeshBumpColorize.fx"
            and report["material"].get("technique") == "t0" and report["material"].get("pass_name") == "t0_p0"
            and report["material"].get("render_pass") == "opaque" and report["material"].get("bindings") == 8,
            "Hull material selector")
    require(report.get("receiving_materials") == {"normal": 3, "receiver_disabled": 2, "variant_failures": 0},
            "receiving variants")
    require(report.get("submitted_instances") == 2, "submitted instances")

    # Run configuration: the label decides whether the receiver casts and
    # which upload route it takes, and every other diagnostic flag must be at
    # its declared default.
    scene = report.get("scene", {})
    receiver = scene.get("receiver", {})
    require(label in RECEIVER_CASTS and receiver.get("casts_shadows") is RECEIVER_CASTS.get(label),
            "receiver casting flag contradicts run label")
    upload = report.get("upload", {})
    route = upload.get("route")
    require(route in ROUTES and route == UPLOAD_ROUTES.get(label), "upload route contradicts run label")
    require(upload.get("surface_sha256") == fixture["surface_sha256"]
            and upload.get("model_bones") == fixture["model_bones"], "upload surface pin")
    # The production adapter accepts a skin pose only for a skinned upload.
    require(upload.get("skin_pose_probe") in ("accepted", "rejected")
            and (upload.get("skin_pose_probe") == "accepted") == (route == "rigid_skinned"),
            "upload route evidence")
    require(isinstance(upload.get("mesh_bone"), int)
            and (upload["mesh_bone"] >= 0) == (route == "rigid_skinned"), "upload mesh bone")
    require(close(receiver.get("translation"), (0.0, 0.0, 0.0)), "receiver translation")
    caster = scene.get("caster", {})
    require(caster.get("sun_distance") == fixture["caster_sun_distance"]
            and caster.get("lateral_offset") == fixture["caster_lateral_offset"], "declared caster placement")
    require(close(caster.get("translation"), fixture["caster_translation"]), "declared caster translation")
    toward = report.get("lighting", {}).get("toward_light")
    require(isinstance(toward, list) and len(toward) == 3 and abs(toward[0]) < 0.01
            and 0.70 < toward[1] < 0.72 and 0.70 < toward[2] < 0.72, "source-backed sun direction")
    camera = report.get("camera", {})
    require(camera.get("width") == WIDTH and camera.get("height") == HEIGHT and camera.get("fov") == 45,
            "camera size/FOV")
    require(close(camera.get("eye"), fixture["eye"]) and close(camera.get("target"), fixture["target"])
            and close(camera.get("up"), fixture["up"])
            and close([camera.get("near"), camera.get("far")], (fixture["near"], fixture["far"])),
            "declared camera")
    shadows = report.get("shadows", {})
    require(shadows.get("projection") == "orthogonal" and shadows.get("atlas_size") == 4096
            and isinstance(shadows.get("max_distance"), (int, float))
            and isinstance(shadows.get("far_distance"), (int, float))
            and shadows["max_distance"] * 0.8 >= shadows["far_distance"], "fade-free shadow coverage")
    require(shadows.get("max_distance") == fixture["max_distance"]
            and close([shadows.get("far_distance")], (fixture["far_distance"],)), "fixture-derived max distance")
    require(shadows.get("bias_policy") == "production_space" and shadows.get("bias") == 2
            and shadows.get("normal_bias") == 5, "production space bias")
    backend = report.get("backend", {})
    require(bool(backend.get("version")) and backend.get("method") == "forward_plus"
            and bool(backend.get("adapter")) and bool(backend.get("api")), "backend")

    selection = report.get("view_selection", {})
    candidates = selection.get("candidates", [])
    qualifying = [(min(c["lit"], c["shadowed"]), i) for i, c in enumerate(candidates)
                  if c.get("lit", 0) >= MIN_MASK_PIXELS and c.get("shadowed", 0) >= MIN_MASK_PIXELS]
    require(len(candidates) == 5 and bool(qualifying)
            and selection.get("selected") == max(qualifying, key=lambda item: (item[0], -item[1]))[1],
            "predeclared view selection rule")
    require(selection.get("selected") == fixture["selected"] and len(candidates) == len(fixture["candidates"])
            and all(close(c.get("view"), view, 1e-6) and c.get("lit") == lit and c.get("shadowed") == shadowed
                    for c, (view, lit, shadowed) in zip(candidates, fixture["candidates"])),
            "declared views")

    masks = report.get("masks", {})
    require(masks.get("sha256") == fixture["mask_sha256"], "mask SHA-256 pin")
    require(masks.get("lit") == fixture["lit"] and masks.get("shadowed") == fixture["shadowed"], "mask count pin")
    mask_path = directory / "masks.pgm"
    classes = None
    try:
        classes = read_pgm(mask_path)
        require(hashlib.sha256(mask_path.read_bytes()).hexdigest() == masks.get("sha256"), "mask SHA-256")
        require(classes.count(1) == masks.get("lit") and classes.count(2) == masks.get("shadowed"),
                "mask counts")
        if classes.count(1) < MIN_MASK_PIXELS or classes.count(2) < MIN_MASK_PIXELS:
            errors.append("mask coverage")
            classes = None
    except (OSError, ValueError) as exc:
        errors.append("mask: " + str(exc))

    if classes is None:
        return errors, None
    captures = report.get("captures", [])
    if [c.get("name") for c in captures if isinstance(c, dict)] != list(NAMES):
        return errors + ["capture sequence"], None
    pixels = {}
    measured = {}
    for capture in captures:
        name = capture["name"]
        require(capture.get("png") == name + ".png", name + " filename")
        try:
            pixels[name] = read_png(directory / (name + ".png"))
        except (OSError, ValueError, zlib.error) as exc:
            errors.append(name + " PNG: " + str(exc))
            continue
        measured[name] = {"receiver": mask_mean(pixels[name], classes, 2),
                          "control": mask_mean(pixels[name], classes, 1)}
        for region, values in measured[name].items():
            declared = capture.get(region, {})
            require(values["pixels"] == declared.get("pixels"), name + " " + region + " pixel count")
            listed = declared.get("rgb")
            require(isinstance(listed, list) and len(listed) == 3
                    and all(abs(a - b) <= 0.02 for a, b in zip(listed, values["rgb"])),
                    name + " " + region + " decoded mean")
    if len(measured) != len(NAMES):
        return errors, None

    on, off = measured["on"], measured["off"]
    darkening = luma(off["receiver"]) - luma(on["receiver"])
    control_shift = abs(luma(off["control"]) - luma(on["control"]))
    negatives = {}
    for name in NAMES[2:]:
        reference = on if name == "restored_on" else off
        negatives[name] = {
            "receiver": abs(luma(measured[name]["receiver"]) - luma(reference["receiver"])),
            "control": abs(luma(measured[name]["control"]) - luma(off["control"])),
        }
    means_pass = (darkening >= DARKENING and control_shift <= CONTROL
                  and all(v["receiver"] <= CONTROL and v["control"] <= CONTROL for v in negatives.values()))
    receiver_hits = sum(1 for index in mask_indices(classes, 2)
                        if luma_at(pixels["off"], index) - luma_at(pixels["on"], index) >= PIXEL_DARKENING)
    control_hits = sum(1 for index in mask_indices(classes, 1)
                       if abs(luma_at(pixels["off"], index) - luma_at(pixels["on"], index)) <= PIXEL_STABLE)
    receiver_fraction = receiver_hits / classes.count(2)
    control_fraction = control_hits / classes.count(1)
    missing = measured["missing_caster"]
    verdict = {
        "darkening": darkening,
        "control_shift": control_shift,
        "negatives": negatives,
        "means_controls_pass": means_pass,
        "receiver_agreement": receiver_fraction,
        "control_agreement": control_fraction,
        "geometric_agreement_pass": receiver_fraction >= RECEIVER_AGREEMENT
        and control_fraction >= CONTROL_AGREEMENT,
        # Receiver darkened without any caster, and the lit control moves: the
        # Hull shadows itself in the shadow map.
        "self_shadow_signature": luma(off["receiver"]) - luma(missing["receiver"]) > CONTROL
        and control_shift > CONTROL,
    }
    failures = report.get("failures")
    require(isinstance(failures, list), "failures list")
    if isinstance(failures, list):
        require(report.get("status") == ("passed" if not failures else "failed"), "status/failures")
        require((not failures) == means_pass, "probe verdict contradicts decoded pixels")
        require(all(any(text in failure for text in CRITERIA_FAILURES) for failure in failures),
                "probe failed for a non-criteria reason")
    return errors, verdict


def write_review_images(directory, gain=12):
    """Write an amplified on/off luma drop and a mask overlay next to a run's PNGs.

    Review aids only (they stay in the ignored output directory). The drop image
    is (off - on) luma x gain, clipped at 0. The overlay shows the on frame in
    grey, receiver-mask pixels that darken by PIXEL_DARKENING in green and misses
    in red, and lit-control pixels within PIXEL_STABLE in blue and moved ones in
    orange."""
    classes = read_pgm(directory / "masks.pgm")
    on, off = read_png(directory / "on.png"), read_png(directory / "off.png")
    drop_image = bytearray(WIDTH * HEIGHT * 4)
    overlay = bytearray(WIDTH * HEIGHT * 4)
    for index, value in enumerate(classes):
        drop = luma_at(off, index) - luma_at(on, index)
        level = max(0, min(255, round(drop * gain)))
        drop_image[index * 4:index * 4 + 4] = bytes((level, level, level, 255))
        grey = round(luma_at(on, index))
        if value == 2:
            colour = (0, 200, 0) if drop >= PIXEL_DARKENING else (220, 0, 0)
        elif value == 1:
            colour = (40, 90, 255) if abs(drop) <= PIXEL_STABLE else (255, 150, 0)
        else:
            colour = (grey, grey, grey)
        overlay[index * 4:index * 4 + 4] = bytes((*colour, 255))
    write_png(directory / f"on_off_drop_x{gain}.png", drop_image)
    write_png(directory / "mask_overlay.png", overlay)


def compare_routes(rigid, unskinned):
    """Cross-image metrics between two matched runs, each (report, directory)."""
    (rigid_report, rigid_dir), (plain_report, plain_dir) = rigid, unskinned
    classes = read_pgm(rigid_dir / "masks.pgm")
    result = {
        "masks_identical": (rigid_dir / "masks.pgm").read_bytes() == (plain_dir / "masks.pgm").read_bytes(),
        "surface_identical": rigid_report["upload"]["surface_sha256"] == plain_report["upload"]["surface_sha256"],
        "routes": [rigid_report["upload"]["route"], plain_report["upload"]["route"]],
        "captures": {},
    }
    for name in NAMES:
        a = read_png(rigid_dir / (name + ".png"))
        b = read_png(plain_dir / (name + ".png"))
        # Rows whose bytes match hold no differing pixel; only the others are compared pixel by pixel.
        stride = WIDTH * 4
        differing = [i for row in range(HEIGHT) if a[row * stride:(row + 1) * stride] != b[row * stride:(row + 1) * stride]
                     for i in range(row * WIDTH, (row + 1) * WIDTH) if a[i * 4:i * 4 + 3] != b[i * 4:i * 4 + 3]]
        result["captures"][name] = {
            "png_sha256": [hashlib.sha256((rigid_dir / (name + ".png")).read_bytes()).hexdigest(),
                           hashlib.sha256((plain_dir / (name + ".png")).read_bytes()).hexdigest()],
            "differing_pixels": len(differing),
            "differing_in_masks": sum(1 for i in differing if classes[i]),
            "receiver_differing_pixels": sum(1 for i in differing if classes[i] == 2),
            "control_differing_pixels": sum(1 for i in differing if classes[i] == 1),
            "max_control_luma_difference": max(
                (abs(luma_at(a, i) - luma_at(b, i)) for i in differing if classes[i] == 1), default=0.0),
            "max_luma_difference": max((abs(luma_at(a, i) - luma_at(b, i)) for i in differing), default=0.0),
            "receiver_luma_difference": luma(mask_mean(a, classes, 2)) - luma(mask_mean(b, classes, 2)),
            "control_luma_difference": luma(mask_mean(a, classes, 1)) - luma(mask_mean(b, classes, 1)),
        }
    result["matched"] = (result["masks_identical"] and result["surface_identical"]
                         and result["routes"] == list(ROUTES)
                         and all(c["differing_pixels"] <= ROUTE_MAX_DIFFERING_PIXELS
                                 and c["receiver_differing_pixels"] == 0
                                 and c["control_differing_pixels"] <= ROUTE_MAX_CONTROL_DIFFERING_PIXELS
                                 and c["max_control_luma_difference"] <= ROUTE_MAX_CONTROL_LUMA_DIFFERENCE
                                 for c in result["captures"].values()))
    return result


def sample(directory, self_shadow=False, shift=0, route="rigid_skinned"):
    """Synthetic evidence in the probe's exact format (no private data).

    Everything but the mask bytes mirrors HULL_FIXTURE, so the mask SHA-256 pin
    is the only pin a synthetic run cannot meet; shift moves the receiver mask."""
    classes = bytearray(WIDTH * HEIGHT)
    for value, left, count in ((1, 100, HULL_FIXTURE["lit"]), (2, 300 + shift, HULL_FIXTURE["shadowed"])):
        for i in range(count):
            classes[(200 + i // 100) * WIDTH + left + i % 100] = value
    write_pgm(directory / "masks.pgm", classes)
    captures = []
    for name in NAMES:
        pixels = bytearray(bytes((18, 22, 33, 255)) * (WIDTH * HEIGHT))
        shadowed = name in ("on", "restored_on")
        for index in (i for wanted in (1, 2) for i in mask_indices(classes, wanted)):
            value = classes[index]
            level = 140
            if value == 2 and (shadowed or (self_shadow and name in ("missing_caster", "casting_disabled"))):
                level = 100
            if value == 1 and self_shadow and name not in ("off", "receiving_disabled"):
                level = 130
            pixels[index * 4:index * 4 + 4] = bytes((level, level, level, 255))
        write_png(directory / (name + ".png"), pixels)
        captures.append({"name": name, "png": name + ".png",
                         "receiver": mask_mean(pixels, classes, 2), "control": mask_mean(pixels, classes, 1)})
    failures = []
    if self_shadow:
        failures = ["lit Hull control changed", "missing_caster receiver contradicts on/off control"]
    f = HULL_FIXTURE
    return {
        "schema": "eawr-hull-asset-shadow-probe-v1", "status": "failed" if failures else "passed",
        "source": "alo_viewer_default", "policy": "sh",
        "asset": {"model": "data/art/models/rebel_mon_calamari_mc_50.alo", "model_sha256": MODEL_SHA256,
                  "texture": "data/art/textures/Rebel_Mon_Calamari_Tide.dds", "texture_sha256": TEXTURE_SHA256,
                  "mesh": "Hull", "vertices": 20084, "triangles": 13094},
        "material": {"program": "MeshBumpColorize.fx", "technique": "t0", "pass_name": "t0_p0",
                     "render_pass": "opaque", "bindings": 8},
        "scene": {"receiver": {"translation": [0, 0, 0], "casts_shadows": self_shadow},
                  "caster": {"translation": list(f["caster_translation"]),
                             "sun_distance": f["caster_sun_distance"], "lateral_offset": f["caster_lateral_offset"]}},
        "lighting": {"toward_light": [0, 0.7071068, 0.7071068]},
        "camera": {"width": WIDTH, "height": HEIGHT, "eye": list(f["eye"]), "target": list(f["target"]),
                   "up": list(f["up"]), "fov": 45, "near": f["near"], "far": f["far"]},
        "view_selection": {"selected": f["selected"], "candidates": [
            {"view": list(view), "lit": lit, "shadowed": shadowed} for view, lit, shadowed in f["candidates"]]},
        "masks": {"file": "masks.pgm", "sha256": hashlib.sha256((directory / "masks.pgm").read_bytes()).hexdigest(),
                  "lit": classes.count(1), "shadowed": classes.count(2)},
        "shadows": {"projection": "orthogonal", "atlas_size": 4096, "max_distance": f["max_distance"],
                    "far_distance": f["far_distance"], "bias_policy": "production_space", "bias": 2,
                    "normal_bias": 5},
        "receiving_materials": {"normal": 3, "receiver_disabled": 2, "variant_failures": 0},
        "submitted_instances": 2,
        "upload": {"route": route, "surface_sha256": f["surface_sha256"],
                   "mesh_bone": 1 if route == "rigid_skinned" else -1, "model_bones": f["model_bones"], "skin_pose_probe": "accepted" if route == "rigid_skinned" else "rejected"},
        "backend": {"version": "4.7.2", "method": "forward_plus", "adapter": "sample", "api": "sample"},
        "captures": captures, "failures": failures,
    }


def pinned(directory):
    """HULL_FIXTURE with a synthetic mask's hash in place of the real one."""
    return dict(HULL_FIXTURE, mask_sha256=hashlib.sha256((directory / "masks.pgm").read_bytes()).hexdigest())


def offset_30(report):
    # A self-consistent diagnostic run; the report measured 86.4% / 93.3% here.
    report["scene"]["caster"]["lateral_offset"] = 30
    report["scene"]["caster"]["translation"][0] = 30.0


def reselected_view(report):
    # Swap two candidates' counts so that the selection rule itself picks view 3.
    candidates = report["view_selection"]["candidates"]
    for key in ("lit", "shadowed"):
        candidates[0][key], candidates[3][key] = candidates[3][key], candidates[0][key]
    report["view_selection"]["selected"] = 3


def unskinned_route(report):
    # A self-consistent unskinned-route run offered under a rigid-route label.
    report["upload"].update(route="unskinned", mesh_bone=-1, skin_pose_probe="rejected")


# Diagnostic or contradictory run configurations. Each leaves the pixels, the
# mask file, every cross-check of the first verifier (eca43c2) and the upload
# route evidence consistent, so only the declared-configuration checks reject
# it, with the given error.
CONFIGURATION_MUTATIONS = {
    "receiver casts under the non-casting label": (
        lambda r: r["scene"]["receiver"].__setitem__("casts_shadows", True),
        "receiver casting flag contradicts run label"),
    "explicit bias 2 / 2": (
        lambda r: r["shadows"].update(bias_policy="explicit", bias=2, normal_bias=2), "production space bias"),
    "adapter-default bias": (
        lambda r: r["shadows"].update(bias_policy="adapter_default", bias=None, normal_bias=None),
        "production space bias"),
    "node-default bias": (
        lambda r: r["shadows"].update(bias_policy="directional_light_node_defaults", bias=0.1, normal_bias=2.0),
        "production space bias"),
    "bias value under the production label": (
        lambda r: r["shadows"].__setitem__("normal_bias", 8), "production space bias"),
    "bias fields missing": (
        lambda r: [r["shadows"].pop(key) for key in ("bias", "normal_bias")], "production space bias"),
    "lateral offset 30": (offset_30, "declared caster placement"),
    "lateral offset label without translation": (
        lambda r: r["scene"]["caster"].__setitem__("lateral_offset", 30), "declared caster placement"),
    "sun distance 250": (
        lambda r: r["scene"]["caster"].update(sun_distance=250, translation=[70.0, 176.7767, 176.7767]),
        "declared caster placement"),
    "mirrored caster translation": (
        lambda r: r["scene"]["caster"]["translation"].__setitem__(0, -70.0), "declared caster translation"),
    "max distance x2": (lambda r: r["shadows"].__setitem__("max_distance", 1230), "fixture-derived max distance"),
    "moved camera eye": (lambda r: r["camera"]["eye"].__setitem__(0, 300.0), "declared camera"),
    "moved camera target": (lambda r: r["camera"]["target"].__setitem__(1, 10.0), "declared camera"),
    "reselected view": (reselected_view, "declared views"),
    "unskinned route under the rigid label": (unskinned_route, "upload route contradicts run label"),
    "changed uploaded surface": (
        lambda r: r["upload"].__setitem__("surface_sha256", "e" * 64), "upload surface pin"),
}
