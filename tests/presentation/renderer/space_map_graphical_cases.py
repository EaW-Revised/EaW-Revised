"""Cases for test_space_map_mode; collected by its legacy facade."""

import xml.etree.ElementTree as ET

from space_map_test_support import (
    ALDERAAN_BLOCKER, ALDERAAN_SPACE, COVERAGE, NONE,
    REFERENCE_MAPS, ROOT, SPACE_STATUSES, SpaceMapRunner,
    _reject_constant, decode_png, fixture, json,
    math, mode_source, os, pathlib,
    read, read_pgm, scene_fixture, source_text,
    strict_json, struct, subprocess, sys,
    tempfile, unittest, zlib,
)


class SpaceGraphicalCases:
    def test_compiled_scene_material_aliases(self):
        with tempfile.TemporaryDirectory(prefix="eawr-compiled-scene-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            model = root / "GameData/Data/Art/Models" / fixture.SKY_MODEL
            images = {}
            additive = [[("Color", (0.25, 0.5, 0.75, 1.0)),
                         ("UVScrollRate", (0.0, 0.0, 0.0, 0.0))]] * 2
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable)
            for shader in ("MeshGloss.fx", "MeshGloss.fxo", "MeshAdditive.fx", "MeshAdditive.fxo"):
                name = shader.replace(".", "-")
                fields = additive if "Additive" in shader else fixture.MESHGLOSS
                model.write_bytes(fixture.alo_bytes([shader, shader], materials=fields))
                report_path = directory / f"{name}.json"
                capture = directory / f"{name}.png"
                completed = subprocess.run([
                    executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-game-root", str(root), "--eawr-unit", "data/art/models/" + fixture.SKY_MODEL,
                    # Render -Z points toward asset +Y; face both fixture normals.
                    "--eawr-unit-times", "0", "--eawr-unit-view", "-0.8,0.1,-1",
                    "--eawr-report", str(report_path), "--eawr-capture", str(capture)],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                report = strict_json(report_path.read_text(encoding="utf-8"))
                self.assertEqual(report["mode"], "unit")
                self.assertEqual(report["status"], "captured", report["failure"])
                self.assertEqual(len(report["surfaces"]), 2)
                self.assertTrue(all(item["effect_missing"] is False for item in report["surfaces"]))
                self.assertEqual(report["skipped"], [])
                images[shader] = decode_png(capture.read_bytes())
            for family in ("MeshGloss", "MeshAdditive"):
                self.assertEqual(images[family + ".fx"], images[family + ".fxo"])
                width, height, pixels = images[family + ".fx"]
                self.assertGreater(sum(pixels[y][x] != pixels[0][0]
                                       for y in range(0, height, 8) for x in range(0, width, 8)), 500,
                                   "generic alias identity requires visible unit pixels")

    def test_compiled_scene_additive2x(self):
        with tempfile.TemporaryDirectory(prefix="eawr-scene-additive2x-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            model = root / "GameData/Data/Art/Models" / fixture.SKY_MODEL
            images = {}
            additive = [[("Color", (0.25, 0.5, 0.75, 1.0)),
                         ("UVScrollRate", (0.0, 0.0, 0.0, 0.0))]] * 2
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable)
            for shader in ("MeshAdditive.fx", "MeshAdditive2x.fxo"):
                name = shader.replace(".", "-")
                fields = additive if "Additive" in shader else fixture.MESHGLOSS
                model.write_bytes(fixture.alo_bytes([shader, shader], materials=fields))
                report_path = directory / f"{name}.json"
                capture = directory / f"{name}.png"
                completed = subprocess.run([
                    executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-game-root", str(root), "--eawr-unit", "data/art/models/" + fixture.SKY_MODEL,
                    # Render -Z points toward asset +Y; face both fixture normals.
                    "--eawr-unit-times", "0", "--eawr-unit-view", "-0.8,0.1,-1",
                    "--eawr-report", str(report_path), "--eawr-capture", str(capture)],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                report = strict_json(report_path.read_text(encoding="utf-8"))
                self.assertEqual(report["mode"], "unit")
                self.assertEqual(report["status"], "captured", report["failure"])
                self.assertEqual(len(report["surfaces"]), 2)
                self.assertTrue(all(item["effect_missing"] is False for item in report["surfaces"]))
                self.assertEqual(report["skipped"], [])
                images[shader] = decode_png(capture.read_bytes())
            baseline = images["MeshAdditive.fx"][2]
            doubled = images["MeshAdditive2x.fxo"][2]
            width, height, _ = images["MeshAdditive.fx"]
            background = baseline[0][0]
            tested = 0
            saturated = 0
            for y in range(0, height, 8):
                for x in range(0, width, 8):
                    before, after = baseline[y][x], doubled[y][x]
                    if before == background:
                        continue
                    for value, actual, clear in zip(before, after, background):
                        expected = min(255, clear + 2 * (value - clear))
                        self.assertAlmostEqual(actual, expected, delta=2)
                        saturated += expected == 255
                    tested += 1
            self.assertGreater(tested, 500, "doubled additive must draw visible generic surfaces")
            self.assertGreater(saturated, 100, "bright samples exercise saturation after sampling")

    def test_scene_specular_glow_colorize(self):
        with tempfile.TemporaryDirectory(prefix="eawr-scene-specglow-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            data = root / "GameData/Data/Art"
            model = data / "Models" / fixture.SKY_MODEL
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable)

            def texture(name, rgba):
                # An original constant 32-bit TGA; top-left origin, eight alpha bits.
                header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 4, 4, 32, 0x28)
                r, g, b, a = rgba
                (data / "Textures" / name).write_bytes(header + bytes((b, g, r, a)) * 16)

            def texture_binding(name, path):
                return scene_fixture._chunk(0x10105, scene_fixture._mini(1, scene_fixture._cstring(name))
                                            + scene_fixture._mini(2, scene_fixture._cstring(path)))

            images = {}
            for name, normal_alpha, emission, specular in (
                    ("glow-zero", 0, (0.25, 0.5, 0.75), (0, 0, 0, 8)),
                    ("glow-half", 128, (0.25, 0.5, 0.75), (0, 0, 0, 8)),
                    ("spec-wide", 0, (0.0, 0.0, 0.0), (64, 32, 16, 8)),
                    ("spec-narrow", 0, (0.0, 0.0, 0.0), (64, 32, 16, 255))):
                texture("eawr_normal.tga", (128, 128, 255, normal_alpha))
                texture("eawr_specular.tga", specular)
                fields = [[("Diffuse", (0.0, 0.0, 0.0)), ("Emissive", emission)]] * 2
                contents = fixture.alo_bytes(["MeshBumpSpecGlowColorize.fx"] * 2, materials=fields)
                # Each fixture material owns its distinct auxiliary samplers.
                extra = texture_binding("NormalTexture", "eawr_normal.tga")
                extra += texture_binding("SpecularTexture", "eawr_specular.tga")
                # Append fields inside each existing material container, updating
                # only that container length (the ALO top-level meshes also need
                # their enclosing length). Rebuild bounded chunks recursively.
                def add_samplers(blob):
                    result = b""
                    offset = 0
                    while offset < len(blob):
                        kind, size = struct.unpack_from("<II", blob, offset)
                        body = blob[offset + 8:offset + 8 + (size & 0x7fffffff)]
                        if kind == 0x10100:
                            body += extra
                        elif kind == 0x400:
                            body = add_samplers(body)
                        result += struct.pack("<II", kind, len(body) | (size & 0x80000000)) + body
                        offset += 8 + (size & 0x7fffffff)
                    return result
                fixture.write_effect_presence(root / "GameData/Data", ["MeshBumpSpecGlowColorize.fx"])
                model.write_bytes(add_samplers(contents))
                report_path = directory / f"{name}.json"
                capture = directory / f"{name}.png"
                completed = subprocess.run([
                    executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-game-root", str(root), "--eawr-unit", "data/art/models/" + fixture.SKY_MODEL,
                    "--eawr-unit-times", "0", "--eawr-unit-view", "-0.8,0.1,-1",
                    "--eawr-report", str(report_path), "--eawr-capture", str(capture)],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                report = strict_json(report_path.read_text(encoding="utf-8"))
                self.assertEqual(report["status"], "captured", report["failure"])
                self.assertEqual(len(report["surfaces"]), 2)
                self.assertTrue(all(item["effect_missing"] is False for item in report["surfaces"]))
                self.assertEqual(report["skipped"], [])
                images[name] = decode_png(capture.read_bytes())
            width, height, half = images["glow-half"]
            zero = images["glow-zero"][2]
            wide, narrow = images["spec-wide"][2], images["spec-narrow"][2]
            visible = highlight = 0
            for y in range(0, height, 8):
                for x in range(0, width, 8):
                    if half[y][x] != half[0][0]:
                        for actual, expected in zip(half[y][x], (32, 64, 96)):
                            self.assertAlmostEqual(actual, expected, delta=2)
                        self.assertLessEqual(max(zero[y][x]), 2,
                                             "zero normal alpha gates emission after lighting")
                        visible += 1
                    highlight += sum(wide[y][x]) > sum(narrow[y][x]) + 8
            self.assertGreater(visible, 500, "emission controls need visible model pixels")
            self.assertGreater(highlight, 100, "specular alpha must change highlight width")

    def test_scene_skinned_additive_alpha(self):
        with tempfile.TemporaryDirectory(prefix="eawr-scene-rskin-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            models = root / "GameData/Data/Art/Models"
            mini, chunk = scene_fixture._mini, scene_fixture._chunk
            # Mover drives palette slot 0; both meshes attach to a different,
            # static Anchor. A rigid-only upload cannot pass the motion guard.
            bones = (("Root", -1, True), ("Mover", 0, True), ("Anchor", 0, True))
            metadata = (mini(4, scene_fixture._cstring("Mover")) + mini(5, struct.pack("<I", 1))
                        + mini(6, struct.pack("<3f", 0, 0, 0))
                        + mini(7, struct.pack("<3f", 80 / 65535, 0, 0))
                        + mini(8, struct.pack("<3f", 1, 1, 1))
                        + mini(9, struct.pack("<3f", 0, 0, 0)))
            positions = b"".join(struct.pack("<3H", x, 0, 0) for x in (0, 65535, 0))
            track = chunk(0x1002, chunk(0x1003, metadata) + chunk(0x1004, positions), True)
            info = mini(1, struct.pack("<I", 3)) + mini(2, struct.pack("<f", 2.0))
            info += mini(3, struct.pack("<I", 1))
            (models / "eawr_skin_motion.ala").write_bytes(chunk(0x1000, chunk(0x1001, info) + track, True))

            def add_palette(blob):
                result = b""
                offset = 0
                while offset < len(blob):
                    kind, size = struct.unpack_from("<II", blob, offset)
                    length = size & 0x7fffffff
                    body = blob[offset + 8:offset + 8 + length]
                    if kind == 0x10000:
                        body += chunk(0x10006, struct.pack("<I", 1))
                    elif kind == 0x400:
                        body = add_palette(body)
                    result += struct.pack("<II", kind, len(body) | (size & 0x80000000)) + body
                    offset += 8 + length
                return result

            additive = [[("Color", (0.25, 0.5, 0.75, 1.0)),
                         ("UVScrollRate", (0.0, 0.0, 0.0, 0.0))]] * 2
            alpha = [[("Diffuse", (0.25, 0.5, 0.75, 0.5)), ("Emissive", (0.1, 0.05, 0.025)),
                      ("Specular", (0.25, 0.25, 0.25))]] * 2
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable)
            images = {}
            for shader in ("MeshAdditive.fx", "RSkinAdditive.fx", "BatchMeshAlpha.fx", "RSkinAlpha.fx"):
                name = shader.replace(".", "-")
                fields = additive if "Additive" in shader else alpha
                model = fixture.alo_bytes([shader] * 2, bones=bones, materials=fields)
                fixture.write_effect_presence(root / "GameData/Data", [shader])
                (models / fixture.SKY_MODEL).write_bytes(add_palette(model))
                report_path = directory / f"{name}.json"
                capture = directory / f"{name}.png"
                completed = subprocess.run([
                    executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-game-root", str(root), "--eawr-unit", "data/art/models/" + fixture.SKY_MODEL,
                    "--eawr-animation", "data/art/models/eawr_skin_motion.ala",
                    "--eawr-unit-times", "0,0.5", "--eawr-unit-view", "-0.8,0.1,-1",
                    "--eawr-report", str(report_path), "--eawr-capture", str(capture)],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                report = strict_json(report_path.read_text(encoding="utf-8"))
                self.assertEqual(report["status"], "captured", report["failure"])
                self.assertEqual(report["skipped"], [])
                self.assertEqual(len(report["surfaces"]), 2)
                self.assertTrue(all(item["effect_missing"] is False for item in report["surfaces"]))
                self.assertTrue(all(item["route"] == "palette" for item in report["surfaces"]))
                self.assertEqual(len(report["frames"]), 2)
                self.assertNotEqual(report["frames"][0]["capture_sha256"],
                                    report["frames"][1]["capture_sha256"],
                                    "the skinned mover must change actual rendered pixels")
                images[shader] = decode_png(capture.read_bytes())
                width, height, pixels = images[shader]
                self.assertGreater(sum(pixels[y][x] != pixels[0][0]
                                       for y in range(0, height, 8) for x in range(0, width, 8)), 500)
            self.assertEqual(images["MeshAdditive.fx"], images["RSkinAdditive.fx"])
            self.assertEqual(images["BatchMeshAlpha.fx"], images["RSkinAlpha.fx"])

    def test_scene_compiled_shields(self):
        with tempfile.TemporaryDirectory(prefix="eawr-scene-shields-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            data = root / "GameData/Data/Art"
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable)
            images = {}

            def texture(name, rgba):
                header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 4, 4, 32, 0x28)
                r, g, b, a = rgba
                (data / "Textures" / name).write_bytes(header + bytes((b, g, r, a)) * 16)

            def sampler(name, path):
                return scene_fixture._chunk(0x10105, scene_fixture._mini(1, scene_fixture._cstring(name))
                                            + scene_fixture._mini(2, scene_fixture._cstring(path)))

            def add_samplers(blob, extra):
                result = b""
                offset = 0
                while offset < len(blob):
                    kind, size = struct.unpack_from("<II", blob, offset)
                    length = size & 0x7fffffff
                    body = blob[offset + 8:offset + 8 + length]
                    if kind == 0x10100:
                        body += extra
                    elif kind == 0x400:
                        body = add_samplers(body, extra)
                    result += struct.pack("<II", kind, len(body) | (size & 0x80000000)) + body
                    offset += 8 + length
                return result

            for name in ("additive", "shield", "wave", "distort", "rim-full", "rim-half"):
                if name.startswith("rim"):
                    shader = "MeshShieldFarseer.fxo"
                    fields = [("Atmosphere", (0.1, 0.2, 0.3, 0.5 if name == "rim-half" else 1.0)),
                              ("AtmospherePower", 0.0)]
                    extra = b""
                else:
                    shader = "MeshAdditive.fx" if name == "additive" else "MeshShield.fxo"
                    fields = [("Color", (0.25, 0.5, 0.75, 0.0)), ("UVScrollRate", (0.0, 0.0, 0.0, 0.0)),
                              ("EdgeBrightness", 1.0), ("BaseUVScale", 1.0), ("WaveUVScale", 1.0),
                              ("DistortUVScale", 1.0), ("BaseUVScrollRate", 0.0),
                              ("WaveUVScrollRate", 0.0), ("DistortUVScrollRate", 0.0)]
                    texture("eawr_wave.tga", (128, 64, 192, 0) if name == "wave" else (255, 255, 255, 0))
                    texture("eawr_distort.tga", (255, 0, 0, 0) if name == "distort" else (0, 0, 0, 0))
                    extra = sampler("WaveTexture", "eawr_wave.tga") + sampler("DistortionTexture", "eawr_distort.tga")
                fixture.write_effect_presence(root / "GameData/Data", [shader])
                model = fixture.alo_bytes([shader] * 2, materials=[fields] * 2)
                (data / "Models" / fixture.SKY_MODEL).write_bytes(add_samplers(model, extra))
                report_path = directory / f"{name}.json"
                capture = directory / f"{name}.png"
                completed = subprocess.run([
                    executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-game-root", str(root), "--eawr-unit", "data/art/models/" + fixture.SKY_MODEL,
                    "--eawr-unit-times", "0", "--eawr-unit-view", "-0.8,0.1,-1",
                    "--eawr-report", str(report_path), "--eawr-capture", str(capture)],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                report = strict_json(report_path.read_text(encoding="utf-8"))
                self.assertEqual(report["status"], "captured", report["failure"])
                self.assertEqual(len(report["surfaces"]), 2)
                self.assertTrue(all(item["effect_missing"] is False for item in report["surfaces"]))
                self.assertEqual(report["skipped"], [])
                images[name] = decode_png(capture.read_bytes())
            self.assertEqual(images["additive"], images["shield"],
                             "white wave and zero distortion keep additive RGB despite zero texture/color alpha")
            width, height, shield = images["shield"]
            wave = images["wave"][2]
            rim, half = images["rim-full"][2], images["rim-half"][2]
            clear = shield[0][0]
            tested = distorted = rim_tested = 0
            for y in range(0, height, 8):
                for x in range(0, width, 8):
                    if shield[y][x] != clear:
                        for value, actual, background, factor in zip(shield[y][x], wave[y][x], clear, (128, 64, 192)):
                            self.assertAlmostEqual(actual, background + (value - background) * factor / 255, delta=2)
                        tested += 1
                    distorted += images["distort"][2][y][x] != shield[y][x]
                    if rim[y][x] != clear:
                        for value, actual, background in zip(rim[y][x], half[y][x], clear):
                            self.assertAlmostEqual(actual, background + 0.25 * (value - background), delta=2)
                        rim_tested += 1
            self.assertGreater(tested, 500, "shield texture product needs visible samples")
            self.assertGreater(distorted, 200, "distortion must shift the sampled energy texture")
            self.assertGreater(rim_tested, 500, "rim alpha is used in both stages on visible surfaces")

    def test_scene_deathstar(self):
        with tempfile.TemporaryDirectory(prefix="eawr-scene-deathstar-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            data = root / "GameData/Data/Art"
            shader = "Planet_Deathstar.fx"
            fixture.write_effect_presence(root / "GameData/Data", [shader])
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable)

            def texture(name, rgba, pattern=False):
                header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 4, 4, 32, 0x28)
                body = bytearray()
                for y in range(4):
                    for x in range(4):
                        r, g, b, a = ((64, 160, 224, 255) if x < 2 else (224, 64, 96, 255)) if pattern else rgba
                        body += bytes((b, g, r, a))
                (data / "Textures" / name).write_bytes(header + body)

            def sampler(name, path):
                return scene_fixture._chunk(0x10105, scene_fixture._mini(1, scene_fixture._cstring(name))
                                            + scene_fixture._mini(2, scene_fixture._cstring(path)))

            def bind_textures(blob):
                output, offset = bytearray(), 0
                while offset < len(blob):
                    kind, size = struct.unpack_from("<II", blob, offset)
                    length = size & 0x7fffffff
                    body = blob[offset + 8:offset + 8 + length]
                    if kind == 0x10100:
                        # The fixture owns its final BaseTexture parameter.
                        start = 0
                        while start < len(body):
                            child, count = struct.unpack_from("<II", body, start)
                            if child == 0x10105:
                                body = body[:start]
                                break
                            start += 8 + (count & 0x7fffffff)
                        body += b"".join(sampler(name, file) for name, file in (
                            ("BaseTexture", "eawr_surface.tga"), ("NormalTexture", "eawr_normal.tga"),
                            ("CloudTexture", "eawr_cloud.tga"), ("CloudNormalTexture", "eawr_cloud_normal.tga")))
                    elif kind == 0x400:
                        body = bind_textures(body)
                    output += struct.pack("<II", kind, len(body) | (size & 0x80000000)) + body
                    offset += 8 + length
                return bytes(output)

            def model(fields):
                (data / "Models" / fixture.SKY_MODEL).write_bytes(bind_textures(
                    fixture.alo_bytes([shader] * 2, materials=[fields] * 2)))

            texture("eawr_cloud_normal.tga", (128, 128, 255, 255))
            images = {}
            for name in ("zero", "city", "cloud-mask", "surface", "gamma", "normal"):
                city = (0.3, 0.2, 0.1) if name in ("city", "cloud-mask") else (0.0, 0.0, 0.0)
                fields = [("CityColor", city), ("Atmosphere", (0.0, 0.0, 0.0, 0.0)),
                          ("CloudScrollRate", 0.0), ("gamma", 1.0 if name == "gamma" else 2.2)]
                texture("eawr_surface.tga", (128, 128, 128, 0) if name in ("surface", "gamma", "normal")
                        else (0, 0, 0, 0))
                texture("eawr_normal.tga", (128, 128, 0 if name == "normal" else 255, 128))
                texture("eawr_cloud.tga", (0, 0, 0, 255 if name == "cloud-mask" else 0))
                model(fields)
                report_path, capture = directory / f"{name}.json", directory / f"{name}.png"
                completed = subprocess.run([
                    executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                    "--eawr-game-root", str(root), "--eawr-unit", "data/art/models/" + fixture.SKY_MODEL,
                    "--eawr-unit-times", "0", "--eawr-unit-view", "-0.8,0.1,-1",
                    "--eawr-report", str(report_path), "--eawr-capture", str(capture)],
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                (directory / f"{name}.log").write_text(completed.stdout, encoding="utf-8")
                self.assertEqual(completed.returncode, 0, completed.stdout)
                report = strict_json(report_path.read_text(encoding="utf-8"))
                self.assertEqual(report["status"], "captured", report["failure"])
                self.assertEqual(report["skipped"], [])
                self.assertEqual(len(report["surfaces"]), 2)
                self.assertTrue(all(item["effect_missing"] is False for item in report["surfaces"]))
                images[name] = decode_png(capture.read_bytes())
            self.assertEqual(images["zero"], images["cloud-mask"], "opaque black clouds must mask night city light")
            width, height, zero = images["zero"]
            city = images["city"][2]
            visible_city = sum(max(zero[y][x]) <= 2 and sum(city[y][x]) > 8
                               for y in range(0, height, 8) for x in range(0, width, 8))
            self.assertGreater(visible_city, 100, "black base alpha must not suppress visible night city light")
            for control in ("gamma", "normal"):
                changed = sum(images["surface"][2][y][x] != images[control][2][y][x]
                              for y in range(0, height, 8) for x in range(0, width, 8))
                self.assertGreater(changed, 100, control + " must change the visible surface")

            # Exercise the real planet-placement ownership route from outside.
            planet = "EAWR_SPACE_DEATHSTAR"
            objects = data.parent / "XML" / "eawr_space_objects.xml"
            objects.write_text(objects.read_text().replace("</SpaceProps>",
                f'<SpaceProp Name="{planet}"><Space_Model_Name>{fixture.SKY_MODEL}</Space_Model_Name>'
                '<In_Background>Yes</In_Background></SpaceProp></SpaceProps>'))
            outside_camera = "-900,80,900,-400,0,50,0,1,0,60,1,5000"

            # Isolate the independent ambient uniform from SH fill and the sun.
            # The invented TED carries a complete finite three-light record.
            def ambient_map(blob, color):
                output, offset = bytearray(), 0
                fields = b"".join(scene_fixture._mini(index, struct.pack("<3f", *(color if index == 4 else (0, 0, 0))))
                                  for index in range(5))
                fields += b"".join(scene_fixture._mini(index, struct.pack("<f", 1.0 if index < 8 else 0.0))
                                   for index in range(5, 14))
                while offset < len(blob):
                    kind, size = struct.unpack_from("<II", blob, offset)
                    length = size & 0x7fffffff
                    body = blob[offset + 8:offset + 8 + length]
                    if kind == 6:
                        # No camera-centred sky: the two single-sided panels
                        # belong to the outside planet placement below.
                        minis, cursor = bytearray(), 0
                        while cursor < len(body):
                            field, count = struct.unpack_from("<BB", body, cursor)
                            if field not in (0x19, 0x1a):
                                minis += body[cursor:cursor + 2 + count]
                            cursor += 2 + count
                        body = bytes(minis) + fields
                    elif kind == 1200:
                        body = (scene_fixture._mini(0, struct.pack("<I", 700))
                                + scene_fixture._mini(1, struct.pack("<I", fixture.type_crc(planet)))
                                + scene_fixture._mini(4, struct.pack("<3f", 0, 0, 0))
                                + scene_fixture._mini(5, struct.pack("<3f", 0, 0, 0)))
                    elif size & 0x80000000:
                        body = ambient_map(body, color)
                    output += struct.pack("<II", kind, len(body) | (size & 0x80000000)) + body
                    offset += 8 + length
                return bytes(output)

            texture("eawr_surface.tga", (255, 255, 255, 0))
            texture("eawr_normal.tga", (128, 128, 255, 128))
            texture("eawr_cloud.tga", (0, 0, 0, 0))
            model([("CityColor", (0.0, 0.0, 0.0)), ("Atmosphere", (0.0, 0.0, 0.0, 0.0)),
                   ("CloudScrollRate", 0.0), ("light_factor", 0.0), ("fill_factor", 0.0),
                   ("ambient_factor", 1.0)])
            map_file = root / "GameData/Data/Art/Maps/EAWR_SPACE_SYNTHETIC.TED"
            original_map = map_file.read_bytes()
            ambient_images = []
            for name, color in (("ambient-zero", (0.0, 0.0, 0.0)), ("ambient-on", (0.2, 0.1, 0.05))):
                map_file.write_bytes(ambient_map(original_map, color))
                code, report, capture = self._run(root, fixture.MAP_LOGICAL_PATH, directory, name,
                    ("--eawr-space-camera",
                     outside_camera))
                self.assertEqual(code, 0, report["failure"])
                surfaces = [item for item in report["space"]["surfaces"] if item["role"] == "planet" and item["object"] == planet]
                self.assertEqual(len(surfaces), 2)
                self.assertTrue(all(item["status"] == "drawn" and item["route"] == "legacy_mesh"
                                    for item in surfaces), surfaces)
                self.assertEqual(report["space"]["effect_clock"]["surfaces"], 0)
                ambient_images.append(decode_png(capture.read_bytes()))
            width, height, unlit = ambient_images[0]
            lit = ambient_images[1][2]
            ambient_samples = sum(max(unlit[y][x]) <= 2 and sum(lit[y][x]) > 8
                                  for y in range(0, height, 8) for x in range(0, width, 8))
            self.assertGreater(ambient_samples, 100, "the actual ambient input must replace the shader default")
            map_file.write_bytes(ambient_map(original_map, (0.08, 0.08, 0.1)))

            # The environment must upload all auxiliary samplers and drive the cloud clock.
            texture("eawr_surface.tga", (0, 0, 0, 255))
            texture("eawr_normal.tga", (128, 128, 255, 128))
            texture("eawr_cloud.tga", (0, 0, 0, 0), pattern=True)
            model([("CityColor", (0.0, 0.0, 0.0)), ("Atmosphere", (0.0, 0.0, 0.0, 0.0)),
                   ("CloudScrollRate", 0.25)])
            clouds = []
            for offset in (0, 10):
                code, report, capture = self._run(root, fixture.MAP_LOGICAL_PATH, directory, f"cloud-{offset}",
                    ("--eawr-populate", "--eawr-lighting", "hemisphere",
                     "--eawr-space-camera", outside_camera,
                     "--eawr-map-idle-offset", str(offset)))
                self.assertEqual(code, 0, report["failure"])
                surfaces = [item for item in report["space"]["surfaces"] if item["role"] == "planet" and item["object"] == planet]
                self.assertEqual(len(surfaces), 2)
                self.assertTrue(all(item["status"] == "drawn" and item["route"] == "legacy_mesh"
                                    for item in surfaces), surfaces)
                self.assertEqual(report["space"]["effect_clock"]["surfaces"], 2)
                clouds.append(decode_png(capture.read_bytes()))
            moved = sum(max(abs(int(a) - int(b)) for a, b in zip(before, after)) > 2
                        for before_row, after_row in zip(clouds[0][2], clouds[1][2])
                        for before, after in zip(before_row, after_row))
            self.assertGreater(moved, 100, "the existing environment clock must move visible cloud pixels")


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"), "set EAWR_EAW_GAME_ROOT for stock planet companions")
    def test_scene_deathstar_stock_alpha_identity(self):
        # Actual stock models/textures, in an original fixed environment.
        # Paired runs before/after the ambient upload must be pixel-identical.
        sys.path.insert(0, str(ROOT / "tools"))
        from megx import read_index
        game = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        archives = [path for path in (game / "corruption/Data/models.meg", game / "Data/models.meg",
                                     game / "GameData/Data/models.meg") if path.is_file()]
        self.assertTrue(archives)
        names = ("w_planet_deathstar02_ii.alo", "w_planet_deathstar_ii_destroyed.alo")
        with tempfile.TemporaryDirectory(prefix="eawr-stock-alpha-") as temporary:
            directory = pathlib.Path(temporary)
            mod = directory / "mod"
            data = mod / "Data"
            (data / "Art/Maps").mkdir(parents=True)
            (data / "XML").mkdir()
            (data / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>")
            (data / "XML/GameObjectFiles.xml").write_text(
                "<Game_Object_Files><File>eawr_stock_alpha.xml</File></Game_Object_Files>")
            rows = []
            for ordinal, name in enumerate(names):
                blob = None
                for archive in archives:
                    entry = next((entry for entry in read_index(archive)
                                  if entry.name.replace("\\", "/").lower().endswith("/" + name)), None)
                    if entry:
                        with archive.open("rb") as stream:
                            stream.seek(entry.offset)
                            blob = stream.read(entry.size)
                        break
                self.assertIsNotNone(blob, name)
                bounds = []

                def geometry_bounds(chunks):
                    offset = 0
                    while offset < len(chunks):
                        kind, size = struct.unpack_from("<II", chunks, offset)
                        length = size & 0x7fffffff
                        body = chunks[offset + 8:offset + 8 + length]
                        if kind == 0x402:
                            bounds.append(struct.unpack_from("<6f", body, 4))
                        elif size & 0x80000000:
                            geometry_bounds(body)
                        offset += 8 + length
                geometry_bounds(blob)
                self.assertTrue(bounds)
                low = [min(row[i] for row in bounds) for i in range(3)]
                high = [max(row[i + 3] for row in bounds) for i in range(3)]
                radius = max(math.sqrt(sum((high[i] - low[i]) ** 2 for i in range(3))) / 2, 1)
                centre = [(low[i] + high[i]) / 2 for i in range(3)]
                target = (-centre[1], centre[2], -centre[0])
                eye = (target[0] - 2.5 * radius, target[1] + radius, target[2] + 2.5 * radius)
                camera = ",".join(str(value) for value in (*eye, *target, 0, 1, 0, 45, 1, 20 * radius + 5000))
                planet = "EAWR_STOCK_ALPHA_PLANET"
                (data / "XML/eawr_stock_alpha.xml").write_text(
                    f'<SpaceProps><SpaceProp Name="{planet}"><Space_Model_Name>{name}</Space_Model_Name>'
                    '<In_Background>Yes</In_Background></SpaceProp></SpaceProps>')
                colours = ((0.7, 0.2, 0.1), (0.1, 0.2, 0.4), (0.2, 0.1, 0.3),
                           (0.3, 0.4, 0.5), (0.18, 0.09, 0.04))
                fields = b"".join(scene_fixture._mini(i, struct.pack("<3f", *colour))
                                  for i, colour in enumerate(colours))
                fields += b"".join(scene_fixture._mini(i, struct.pack("<f", value))
                                   for i, value in enumerate((1, .5, .6, .3, 1, 2, .2, .4, .8), 5))
                header = scene_fixture._mini(0, struct.pack("<I", 0x0201)) + scene_fixture._mini(1, struct.pack("<I", 2))
                placement = (scene_fixture._mini(0, struct.pack("<I", 700))
                             + scene_fixture._mini(1, struct.pack("<I", fixture.type_crc(planet)))
                             + scene_fixture._mini(4, struct.pack("<3f", 0, 0, 0))
                             + scene_fixture._mini(5, struct.pack("<3f", 0, 0, 0)))
                chunk = scene_fixture._chunk
                main = (chunk(256, chunk(4, chunk(6, fields), True), True)
                        + chunk(258, chunk(1, chunk(1100, chunk(1113, chunk(1200, placement), True), True), True), True))
                (data / "Art/Maps/EAWR_STOCK_ALPHA.TED").write_bytes(
                    struct.pack("<II", 0, len(header)) + header + chunk(1, main, True))
                for policy in ("off", "hemisphere", "sh"):
                    label = f"stock-alpha-{ordinal}-{policy}"
                    code, report, capture = self._run(game, "data/art/maps/eawr_stock_alpha.ted", directory, label,
                        ("--eawr-mod-root", str(mod), "--eawr-populate", "--eawr-lighting", policy,
                         "--eawr-environment", "map", "--eawr-shadows", "off", "--eawr-space-camera", camera))
                    self.assertEqual(code, 0, report["failure"])
                    alpha = [item for item in report["space"]["surfaces"]
                             if item["shader"].lower() == "meshalpha.fx" and item["status"] == "drawn"]
                    self.assertTrue(alpha, (name, report["space"]["surfaces"]))
                    self.assertTrue(all(item["route"] == "legacy_mesh" for item in alpha))
                    width, height, pixels = decode_png(capture.read_bytes())
                    visible = sum(pixels[y][x] != pixels[0][0] for y in range(0, height, 4)
                                  for x in range(0, width, 4))
                    self.assertGreater(visible, 500, "the actual stock planet must be visible")
                    rows.append({"name": label, "model": name, "policy": policy,
                                 "alpha_meshes": [item["mesh"] for item in alpha], "visible_samples": visible})
            (directory / "stock-alpha-case.json").write_text(json.dumps(rows, indent=2))

    def test_compiled_environment_material_routes(self):
        with tempfile.TemporaryDirectory(prefix="eawr-compiled-environment-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture", "meshgloss")
            model = root / "GameData/Data/Art/Models" / fixture.SKY_MODEL
            # The environment applies the object's quarter turn to the sky;
            # look toward source -X to see these source +Y fixture walls.
            options = ("--eawr-space-camera", "0,0,0,-1,0,0,0,1,0,60,1,5000", "--eawr-populate")
            images = {}
            additive = [[("Color", (0.25, 0.5, 0.75, 1.0)),
                         ("UVScrollRate", (0.0, 0.0, 0.0, 0.0))]] * 2
            for shader in ("MeshGloss.fx", "MeshGloss.fxo", "MeshGlossForStarfields.fxo",
                           "MeshAdditive.fx", "MeshAdditive.fxo", "MeshAdditive2x.fxo"):
                fields = additive if "Additive" in shader else fixture.MESHGLOSS
                model.write_bytes(fixture.alo_bytes([shader, shader], materials=fields))
                code, report, capture = self._run(root, fixture.MAP_LOGICAL_PATH, directory,
                                                  shader.replace(".", "-"), options)
                self.assertEqual(code, 0, report["failure"])
                surfaces = [item for item in report["space"]["surfaces"] if item["role"] == "primary_sky"]
                self.assertEqual(len(surfaces), 2)
                self.assertTrue(all(item["status"] == "drawn" for item in surfaces), surfaces)
                images[shader] = decode_png(capture.read_bytes())
            gloss = images["MeshGloss.fx"][2]
            self.assertGreater(sum(gloss[y][x] != gloss[0][0]
                                   for y in range(50, 670, 8) for x in range(50, 1230, 8)), 500,
                               "alias identity requires visible fixture pixels")
            self.assertEqual(images["MeshGloss.fx"], images["MeshGloss.fxo"])
            self.assertEqual(images["MeshGloss.fx"], images["MeshGlossForStarfields.fxo"])
            self.assertEqual(images["MeshAdditive.fx"], images["MeshAdditive.fxo"])
            baseline = images["MeshAdditive.fx"][2]
            doubled = images["MeshAdditive2x.fxo"][2]
            background = baseline[0][0]
            tested = 0
            for y in range(50, 670, 8):
                for x in range(50, 1230, 8):
                    before, after = baseline[y][x], doubled[y][x]
                    if before == background:
                        continue
                    for value, actual, clear in zip(before, after, background):
                        self.assertAlmostEqual(actual, min(255, clear + 2 * (value - clear)), delta=2)
                    tested += 1
            self.assertGreater(tested, 500)

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the sunlit MC80 shadow regression")
    def test_sunlit_mc80_shadow_acne(self):
        # #667: compare the same lit surface with shadows on/off. Dividing
        # stored RGB cancels the hull texture. The preview's default sun
        # exposes the owner-visible band; the map sun lights it differently.
        # The declared patch lies inside that band, away from contact edges.
        with tempfile.TemporaryDirectory(prefix="eawr-mc80-shadow-acne-") as temporary:
            directory = pathlib.Path(temporary)
            config = ROOT / "apps/viewer/project/config"
            tree = ET.parse(config / "coruscant-live-session-camera.xml")
            tree.getroot().find("initial").attrib.update(
                target_x="-4795.853027", target_y="4020.553467", target_height="-290",
                zoom="0", yaw_degrees="0")
            bindings = directory / "space-live-camera-bindings.json"
            bindings.write_text((config / bindings.name).read_text(encoding="utf-8"), encoding="utf-8")
            camera = directory / "camera.xml"
            tree.write(camera, encoding="utf-8", xml_declaration=True)
            options = ("--eawr-populate", "--eawr-live-session", "m2", "--eawr-live-ticks", "1",
                       "--eawr-live-reveal", "on", "--eawr-live-ai", "off", "--eawr-hud", "off",
                       "--eawr-environment", "default", "--eawr-lighting", "sh",
                       "--eawr-map-camera-config", str(camera))
            images = {}
            reports = {}
            for shadow in ("on", "off"):
                code, report, capture = self._run(
                    pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                    "data/art/maps/_mp_space_coruscant.ted", directory, f"mc80-{shadow}",
                    options + ("--eawr-shadows", shadow))
                self.assertEqual(code, 0, report["failure"])
                reports[shadow] = report
                width, height, images[shadow] = decode_png(capture.read_bytes())
                self.assertEqual((width, height), (1280, 720))
            self.assertEqual(reports["on"]["capture_identity"]["camera"],
                             reports["off"]["capture_identity"]["camera"])
            self.assertEqual(reports["on"]["lighting"]["shadow_max_distance"], 8192)
            self.assertTrue(reports["on"]["lighting"]["shadows"])
            self.assertFalse(reports["off"]["lighting"]["shadows"])
            ratios = {}
            for y in range(400, 550):
                for x in range(360, 850):
                    denominator = sum(images["off"][y][x])
                    if denominator >= 96:
                        ratios[x, y] = sum(images["on"][y][x]) / denominator
            changes = [abs(ratio - ratios[neighbour])
                       for (x, y), ratio in ratios.items()
                       for neighbour in ((x + 1, y), (x, y + 1)) if neighbour in ratios]
            self.assertGreater(len(ratios), 10000, "the declared hull patch must be visible")
            fraction = sum(change > 0.04 for change in changes) / len(changes)
            (directory / "acne-metric.json").write_text(json.dumps({
                "rectangle": [360, 400, 850, 550], "pixels": len(ratios),
                "pairs": len(changes), "high_frequency_fraction": fraction}), encoding="utf-8")
            self.assertLess(fraction, 0.02, "sunlit MC80 hull has shadow-induced hatching")
            # A positive control prevents disabling every shadow from passing.
            darkened = sum(sum(images["off"][y][x]) >= 96
                           and sum(images["on"][y][x]) < 0.9 * sum(images["off"][y][x])
                           for y in range(height) for x in range(width))
            self.assertGreater(darkened, 200, "the hull still receives visible shadows")

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant shadow report")
    def test_shadow_report_default_and_debug_ship(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        options = ("--eawr-populate", "--eawr-map-effects", "on", "--eawr-lighting", "sh",
                   "--eawr-environment", "map", "--eawr-shadows", "on",
                   "--eawr-map-camera-config", str(camera))
        # #667: the ordinary and the debug view share the space shadow
        # settings; the debug view only fits the range to the selected ship.
        settings = {"shadow_mode": "parallel_4_splits", "shadow_bias": 0.05,
                    "shadow_normal_bias": 5.0, "shadow_blur": 1.0}
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-shadow-report-") as temporary:
            directory = pathlib.Path(temporary)
            code, ordinary, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                          directory, "ordinary", options)
            self.assertEqual(code, 0, ordinary["failure"])
            lighting = ordinary["lighting"]
            self.assertEqual((lighting["policy"], lighting["composed"], lighting["shadows"], lighting["environment"]),
                             ("sh", True, True, "map"))
            self.assertEqual(lighting["shadow_max_distance"], 8192.0)
            self.assertEqual(lighting["shadow_filter"], "soft_medium")
            self.assertEqual(lighting["shadow_atlas_size"], 4096)
            # 7eb40d84 (#1043) admits the secondary sky's additive vertex-colour material as well.
            self.assertEqual((lighting["shadow_receiving_materials"], lighting["shadow_variant_failures"]), (138, 0))
            for key, value in settings.items():
                self.assertEqual(lighting[key], value, key)
            self.assertNotIn("shadow_scope", lighting)
            code, debug, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                       directory, "debug", options +
                                       ("--eawr-space-place-object", "Star_Destroyer@55"))
            self.assertEqual(code, 0, debug["failure"])
            for key, value in settings.items():
                self.assertEqual(debug["lighting"][key], value, key)
            self.assertLess(debug["lighting"]["shadow_max_distance"], lighting["shadow_max_distance"])
            self.assertEqual(debug["lighting"]["shadow_floor"], lighting["shadow_floor"])
            self.assertEqual(debug["lighting"]["shadow_scope"],
                             "scene directional light (debug evidence only)")
            self.assertEqual(debug["populate"]["debug_ship"]["status"], "composed")


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant hardpoint state run")
    def test_debug_ship_hardpoint_states(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        # Explicit budgets keep the hardpoint test independent of ambient
        # prewarmed allocation; 256/proxy reproduces its original admission.
        options = ("--eawr-populate", "--eawr-map-camera-config", str(camera),
                   "--eawr-space-place-object", "Star_Destroyer@55", "--eawr-map-particle-capacity", "16384",
                   "--eawr-map-attached-capacity", "256")
        hurt = ("HP_Star_Destroyer_Weapon_FL", "HP_Star_Destroyer_Weapon_BL", "HP_Star_Destroyer_Weapon_ML",
                "HP_Star_Destroyer_Shield_Generator")

        def states(state):
            return tuple(item for hardpoint in hurt for item in ("--eawr-space-hardpoint-state", f"{hardpoint}={state}"))

        def ship_emitters(result):
            return {item["identity"].rsplit("/", 1)[-1]: item["status"]
                    for item in result["map_particles"]["placements"] if "#55/proxy#" in item["identity"]}

        # #136: a spawned ship has every hardpoint intact; a destroyed one
        # drops its attached model and shows its damage decal.
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-hardpoints-") as temporary:
            directory = pathlib.Path(temporary)
            code, intact, intact_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                                 directory, "intact", options)
            self.assertEqual(code, 0, intact["failure"])
            ship = next(item for item in intact["populate"]["placements"] if item["object"] == "Star_Destroyer")
            self.assertEqual((ship["hardpoints"], ship["hidden_damage_decal_surfaces"]), (10, 8))
            # Tractor beam and fighter bay attach no model; the engines' model
            # holds only a hidden collision mesh.
            self.assertEqual(intact["populate"]["hardpoints_attached"], 7)
            self.assertEqual(intact["populate"]["debug_ship"]["hardpoint_states"], {})
            # The ship is in the attached-effect plan: its intact hardpoints
            # hide their 20 damage emitters; one other emitter runs.
            attached = intact["populate"]["attached_effects"]
            self.assertEqual((attached["hidden_by_hardpoint_state"], attached["capacity_exhausted"]), (20, 0))
            self.assertEqual(ship_emitters(intact), {"proxy#20": "drawn"})
            # Damaged hardpoints keep their models, hidden decals, and hidden
            # emitters: their capture must match intact exactly.
            code, damaged, damaged_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                                   directory, "damaged", options + states("damaged"))
            self.assertEqual(code, 0, damaged["failure"])
            self.assertEqual((damaged["populate"]["instances"], damaged["populate"]["hardpoints_attached"]),
                             (intact["populate"]["instances"], 7))
            self.assertEqual(damaged["populate"]["attached_effects"]["hidden_by_hardpoint_state"], 20)
            self.assertEqual(ship_emitters(damaged), ship_emitters(intact))
            self.assertEqual(damaged["map_particles"]["live_rids_after_release"], 0)
            self.assertEqual(intact_png.read_bytes(), damaged_png.read_bytes())
            # Back to intact, the hook retains the same art before the first frame.
            code, healed, healed_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "healed",
                                                 options + states("damaged") + states("intact"))
            self.assertEqual(code, 0, healed["failure"])
            self.assertEqual(healed["populate"]["attached_effects"]["hidden_by_hardpoint_state"], 20)
            self.assertEqual(ship_emitters(healed), {"proxy#20": "drawn"})
            self.assertEqual(healed_png.read_bytes(), intact_png.read_bytes())
            code, destroyed, destroyed_png = self._run(
                root, "data/art/maps/_mp_space_coruscant.ted", directory, "destroyed",
                options + ("--eawr-space-hardpoint-state", "HP_Star_Destroyer_Shield_Generator=destroyed",
                           "--eawr-space-hardpoint-state", "hp_star_destroyer_weapon_fl=Damaged"))
            self.assertEqual(code, 0, destroyed["failure"])
            ship = next(item for item in destroyed["populate"]["placements"] if item["object"] == "Star_Destroyer")
            self.assertEqual(ship["hidden_damage_decal_surfaces"], 7)
            self.assertEqual(destroyed["populate"]["hardpoints_attached"], 6)
            self.assertEqual(destroyed["populate"]["debug_ship"]["hardpoint_states"],
                             {"HP_Star_Destroyer_Shield_Generator": "destroyed",
                              "hp_star_destroyer_weapon_fl": "damaged"})
            self.assertEqual(destroyed["populate"]["instances"], intact["populate"]["instances"] - 1)
            # The damaged front-left weapon remains visually intact.
            self.assertEqual(set(ship_emitters(destroyed)), {f"proxy#{proxy}" for proxy in (14, 15, 20)})
            self.assertNotEqual(intact_png.read_bytes(), destroyed_png.read_bytes())
            code, unknown, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "unknown",
                                         options + ("--eawr-space-hardpoint-state", "HP_Nowhere=destroyed"))
            self.assertNotEqual(code, 0)
            self.assertIn("HP_Nowhere is not a hardpoint of Star_Destroyer", unknown["failure"])


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant station marker run")
    def test_station_markers_attach_no_effects(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        options = ("--eawr-populate", "--eawr-map-camera-config", str(camera))

        def emitters(result, record):
            return [item["status"] for item in result["map_particles"]["placements"]
                    if f"#{record}/proxy#" in item["identity"]]

        # #284: Team_00/01_Space_Station (records 48 and 54) are markers and
        # used to attach eb_station_01's 8 p_rb_station_damage and 2
        # p_blink_white_big proxies each: 81 records, 52 admitted.
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-markers-") as temporary:
            directory = pathlib.Path(temporary)
            code, markers, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "markers", options)
            self.assertEqual(code, 0, markers["failure"])
            roles = {item["ordinal"]: item["role"] for item in markers["populate"]["placements"]}
            self.assertEqual((roles[48], roles[54]), ("marker", "marker"))
            attached = markers["populate"]["attached_effects"]
            self.assertEqual((attached["records"], attached["admitted"], attached["hidden_by_hardpoint_state"],
                              attached["capacity_exhausted"]), (59, 32, 0, 0))
            self.assertEqual((emitters(markers, 48), emitters(markers, 54)), ([], []))
            # A real station on the marker hides the 10 damage emitters under
            # its intact hardpoints' Damage_Particles bones, as retail does
            # when it builds the hardpoints; only its blink light runs.
            code, station, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "station",
                                         options + ("--eawr-space-place-object", "Rebel_Star_Base_1@48"))
            self.assertEqual(code, 0, station["failure"])
            attached = station["populate"]["attached_effects"]
            self.assertEqual((attached["records"], attached["admitted"], attached["hidden_by_hardpoint_state"]),
                             (71, 33, 10))
            self.assertEqual(emitters(station, 48), ["drawn"])


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant population fault probe")
    def test_supported_population_upload_failure_fails_run_with_placement(self):
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-upload-fault-") as temporary:
            code, result, _ = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                "data/art/maps/_mp_space_coruscant.ted", pathlib.Path(temporary), "upload-fault",
                ("--eawr-populate", "--eawr-space-populate-reject-upload-test"))
            self.assertNotEqual(code, 0)
            self.assertEqual(result["status"], "failed")
            self.assertIn("supported surface placement uploads failed", result["failure"])
            self.assertIn("placement 0 (Skirmish_Merchant_Dock) surface 0", result["failure"])
            self.assertIn("injected supported surface upload failure", result["failure"])


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant attached-effects run")
    def test_coruscant_attached_effects_tick_in_environment_view(self):
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-effects-") as temporary:
            directory = pathlib.Path(temporary)
            camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
            code, result, capture = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                "data/art/maps/_mp_space_coruscant.ted", directory, "effects",
                ("--eawr-populate", "--eawr-map-camera-config", str(camera),
                 "--eawr-lighting", "sh", "--eawr-shadows", "on"))
            self.assertEqual(code, 0, result["failure"])
            self.assertEqual(result["status"], "space_environment_rendered")
            self.assertGreater(result["map_camera"]["steps"], 0)
            population = result["populate"]
            self.assertGreater(population["team_colour"]["colour_variants"], 0)
            self.assertGreater(population["team_colour"]["drawn_by_status"]["faction_colour"], 0)
            self.assertGreater(population["unit_animation"]["placements_animated"], 0)
            self.assertGreater(population["unit_animation"]["instances_animated"], 0)
            self.assertEqual(population["unit_animation"]["sample_at_capture"], 59)
            effects = result["map_particles"]
            self.assertGreater(effects["attached_records"], 0)
            self.assertGreater(effects["advanced_frames"], 0)
            # A fixed capture holds the effects on the frame-count clock (#186).
            self.assertEqual(effects["clock"], "held")
            self.assertTrue(any(item["status"] == "drawn" for item in effects["placements"]))
            self.assertEqual(effects["live_rids_after_release"], 0)
            self.assertEqual(effects["live_resources_after_release"], 0)
            self.assertTrue(capture.is_file())


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the authored asteroid density")
    def test_coruscant_prewarmed_asteroid_density(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        options = ("--eawr-populate", "--eawr-space-camera",
                   "-356.6,689.44,574.609,-356.6,0,-3.9,0,1,0,55,10,60000")

        def field(report):
            return [p for p in report["map_particles"]["placements"] if "#12/proxy#" in p["identity"]]

        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-pebbles-") as temporary:
            directory = pathlib.Path(temporary)
            code, full, full_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                            directory, "full", options)
            self.assertEqual(code, 0, full["failure"])
            effects = field(full)
            self.assertEqual(len(effects), 2)
            self.assertGreaterEqual(effects[0]["particles"], 825)
            self.assertGreaterEqual(effects[1]["particles"], 395)
            self.assertEqual(full["populate"]["attached_effects"]["capacity_exhausted"], 0)
            for effect in effects:
                self.assertLess(effect["particles"], effect["capacity"])
            code, repeat, repeat_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                                directory, "repeat", options)
            self.assertEqual(code, 0, repeat["failure"])
            self.assertEqual(field(repeat), effects)
            self.assertEqual(full_png.read_bytes(), repeat_png.read_bytes())
            code, limited, limited_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                directory, "limited", options + ("--eawr-map-attached-capacity", "256",
                                                   "--eawr-map-particle-capacity", "8192"))
            self.assertEqual(code, 0, limited["failure"])
            self.assertEqual([p["capacity"] for p in field(limited)], [256, 256])
            self.assertTrue(all(p["particles"] <= 256 for p in field(limited)))
            self.assertEqual(limited["map_particles"]["aggregate_capacity"], 8192)
            self.assertNotEqual(full_png.read_bytes(), limited_png.read_bytes())


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant idle clip run")
    def test_coruscant_props_tumble_through_their_idle_clips(self):
        # #145: the asteroid field and space junk idle clips play from
        # per-placement start frames at their XML playback; five seconds later
        # on the idle clock (--eawr-map-idle-offset 150) they have turned.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        # The south-east asteroid field: mining pad, Asteroid Field Small, junk.
        camera = "1419,271,5380,1920,-150,5380,0,1,0,55,10,60000"
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-idle-") as temporary:
            directory = pathlib.Path(temporary)
            frames = {}
            for offset in (0, 150):
                code, result, capture = self._run(
                    root, "data/art/maps/_mp_space_coruscant.ted", directory, f"idle-{offset}",
                    ("--eawr-populate", "--eawr-space-camera", camera, "--eawr-map-idle-offset", str(offset)))
                self.assertEqual(code, 0, result["failure"])
                animation = result["populate"]["unit_animation"]
                self.assertEqual((animation["clock"], animation["sample_at_capture"], animation["idle_offset"]),
                                 ("held", 59, offset))
                self.assertEqual(animation["idle_sample_failures"], 0)
                hulls = {entry["object"]: entry for entry in animation["bound_idle_hulls"]}
                self.assertEqual(hulls["Asteroid Field Small"]["clip"],
                                 "data/art/models/w_asteroid_mass_small_idle_00.ala")
                self.assertTrue(hulls["Asteroid Field Small"]["loop"])
                self.assertEqual(hulls["Asteroid Field Small"]["rate_mod"], [1, 1])
                # No Loop_Idle_Anim_00: the IDLE behaviour restarts it instead.
                self.assertFalse(hulls["Space_Junk_Small"]["loop"])
                self.assertTrue(hulls["Space_Junk_Small"]["restarts"])
                self.assertTrue(all(entry["random_start"] for entry in hulls.values()))
                frames[offset] = decode_png(capture.read_bytes())
        width, height, before = frames[0]
        self.assertEqual((width, height), (1280, 720))
        _, _, after = frames[150]

        def changed(x_range, y_range):
            return sum(1 for y in y_range for x in x_range
                       if max(abs(a - b) for a, b in zip(before[y][x], after[y][x])) > 40)

        self.assertGreater(changed(range(0, width, 4), range(0, height, 4)), 500,
                           "the asteroids and junk turned between the two idle clock samples")
        # Control: empty sky above the field is the same in both captures.
        self.assertEqual(changed(range(700, 1000, 2), range(0, 60, 2)), 0)


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant nebula clock run")
    def test_coruscant_nebula_moves_on_the_effect_clock(self):
        # #185: Nebula.fx moves only through TIME, a 20 s vertex wave and a UV
        # scroll. The environment view runs it on the idle clips' clock at the
        # retail 0.03 s per tick, so five seconds later on that clock
        # (--eawr-map-idle-offset 150) the nebula has moved, and one offset
        # captured twice is the same image.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        self._nebula_moves(root, "forward_plus")

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the lit live nebula regression")
    def test_coruscant_live_nebula_has_visible_coverage(self):
        # FW-19: the opaque sky must precede additive backdrop layers, even
        # though Godot puts depth_draw_never sky shaders in its alpha pass.
        # Use the owner's live camera policy with an explicit pose aimed at
        # the nebula cluster; the opening fleet pose does not frame the clouds.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-live-session-camera.xml"
        options = ("--eawr-populate", "--eawr-map-effects", "on",
                   "--eawr-live-session", "m2", "--eawr-live-ticks", "1",
                   "--eawr-live-ai", "off", "--eawr-hud", "off",
                   "--eawr-environment", "map", "--eawr-lighting", "sh",
                   "--eawr-shadows", "on", "--eawr-map-camera-config", str(camera),
                   "--eawr-space-camera", "4000,2298,-1872,4000,0,-3800,0,1,0,55,10,60000")
        with tempfile.TemporaryDirectory(prefix="eawr-live-nebula-") as temporary:
            directory = pathlib.Path(temporary)
            for reveal in ("off", "on"):
                with self.subTest(reveal=reveal):
                    code, result, capture = self._run(
                        root, "data/art/maps/_mp_space_coruscant.ted", directory, f"nebula-{reveal}",
                        options + ("--eawr-live-reveal", reveal))
                    self.assertEqual(code, 0, result["failure"])
                    nebulae = [item for item in result["space"]["surfaces"] if item["route"] == "nebula"]
                    self.assertEqual(len(nebulae), 7)
                    self.assertTrue(all(item["status"] == "drawn" for item in nebulae))
                    width, height, pixels = decode_png(capture.read_bytes())
                    self.assertEqual((width, height), (1280, 720))
                    # The warm cloud filaments occupy this rectangle. The
                    # blue starfield and foreground stations cannot satisfy
                    # the coverage floor when the sky overwrites the nebula.
                    coverage = sum(pixels[y][x][0] > 48 and pixels[y][x][0] > pixels[y][x][2]
                                   for y in range(10, 680, 2) for x in range(250, 600, 2))
                    self.assertGreater(coverage, 1000, "lit nebula filaments must contribute visible pixels")

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for other lit nebula backdrops")
    def test_other_nebula_backdrops_move_in_lit_views(self):
        # Frame the authored Endor ion cluster and Bothawui's southeast cloud.
        # A successful upload is insufficient: the animated layer must change
        # pixels after the sky draw, on maps with different sky/nebula assets.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        cases = (("endor", 3, "0,2298,1928,0,0,0,0,1,0,55,10,60000"),
                 ("bothawui", 2, "4500,2298,7428,4500,0,5500,0,1,0,55,10,60000"))
        with tempfile.TemporaryDirectory(prefix="eawr-other-nebula-") as temporary:
            directory = pathlib.Path(temporary)
            for name, count, camera in cases:
                with self.subTest(map=name):
                    images = []
                    for offset in (0, 150):
                        code, result, capture = self._run(
                            root, f"data/art/maps/_mp_space_{name}.ted", directory, f"{name}-{offset}",
                            ("--eawr-populate", "--eawr-map-effects", "off", "--eawr-space-camera", camera,
                             "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                             "--eawr-map-idle-offset", str(offset)))
                        self.assertEqual(code, 0, result["failure"])
                        nebulae = [item for item in result["space"]["surfaces"] if item["route"] == "nebula"]
                        self.assertEqual(len(nebulae), count)
                        self.assertTrue(all(item["status"] == "drawn" for item in nebulae))
                        width, height, pixels = decode_png(capture.read_bytes())
                        self.assertEqual((width, height), (1280, 720))
                        images.append(pixels)
                    changed = sum(max(abs(a - b) for a, b in zip(images[0][y][x], images[1][y][x])) > 24
                                  for y in range(100, 620, 4) for x in range(200, 1080, 4))
                    self.assertGreater(changed, 1000, "lit nebula motion must survive backdrop ordering")


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the galactic planet map markers")
    def test_planet_map_marker_models_are_not_environment(self):
        # #3077: every galactic planet space map places a Death_Star_Position
        # marker whose editor model uses the planet shader. Markers are never
        # drawn in game; the map's own backdrop planet still is.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        with tempfile.TemporaryDirectory(prefix="eawr-planet-marker-") as temporary:
            code, result, _ = self._run(root, "data/art/maps/_space_planet_corellia_01.ted",
                                        pathlib.Path(temporary), "corellia",
                                        ("--eawr-populate", "--eawr-map-effects", "off"))
            self.assertEqual(code, 0, result["failure"])
        surfaces = result["space"]["surfaces"]
        self.assertFalse([item for item in surfaces if item["object"].lower() == "death_star_position"])
        planets = [item for item in surfaces if item["role"].startswith("planet")]
        self.assertTrue(planets and all(item["status"] == "drawn" for item in planets), planets)


    def _place_at(self, ship):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        with tempfile.TemporaryDirectory(prefix="eawr-death-star-") as temporary:
            code, result, _ = self._run(root, "data/art/maps/_space_planet_nalhutta_01.ted",
                                        pathlib.Path(temporary), "nalhutta",
                                        ("--eawr-populate", "--eawr-map-effects", "off",
                                         "--eawr-space-place-at", ship))
            self.assertEqual(code, 0, result["failure"])
        return result

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Nal Hutta Death Star placement")
    def test_background_authored_space_unit_is_drawn_at_its_pose(self):
        # #3935 (BP-75): the Death Star authors In_Background for its tactical unit too. A placed ship is
        # a live simulated object, so it is registered and drawn (the placement run fails with "no
        # supported visible hull" when it is skipped as map environment). It is counted as a backdrop
        # ship and drawn (composed surfaces) at a pose like the one the debug build gave a spawned one
        # (z -3100, below the ship plane), never the #3077 marker. The pose itself is asserted on the
        # live session (live_session_effect_cases.py); this run asserts the placed/backdrop counts only.
        result = self._place_at("Death_Star_II@-1328,-762,-3100,0")
        self.assertEqual(result["populate"]["placed_ships"], 1)
        self.assertEqual(result["populate"]["backdrop_ships"], 1)
        self.assertGreater(result["populate"]["composed"], 0, result["populate"])
        surfaces = result["space"]["surfaces"]
        self.assertTrue(surfaces)
        self.assertFalse([item for item in surfaces if item["object"].lower() == "death_star_position"])

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Nal Hutta ship placement")
    def test_ordinary_space_unit_is_not_a_background_scene_object(self):
        # Negative: a ship without In_Background is a world-scene ship.
        result = self._place_at("Star_Destroyer@0,0,0,0")
        self.assertEqual(result["populate"]["placed_ships"], 1)
        self.assertEqual(result["populate"]["backdrop_ships"], 0)


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant opening pose")
    def test_coruscant_default_pose_is_the_tactical_opening_pose(self):
        # One opening rule: without a map camera config the default view
        # frames local player 1's spawn marker at the Space_Mode defaults; the
        # project config's tactical camera opens on the same target, pitch and
        # yaw, but at distance 1200 instead of FoC's Distance_Default 1000
        # (owner deviation, #337/#348). The config-less view keeps FoC's.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-opening-") as temporary:
            directory = pathlib.Path(temporary)
            code, plain, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "default")
            self.assertEqual(code, 0, plain["failure"])
            code, tactical, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "tactical",
                                          ("--eawr-map-camera-config", str(camera)))
            self.assertEqual(code, 0, tactical["failure"])
        self.assertEqual(plain["space"]["camera"]["source"], "default")
        default = plain["space"]["camera"]["default"]
        self.assertEqual((default["target_kind"], default["target_record"], default["target_type"]),
                         ("start_marker", 55, "Team_01_Spawn_Point_Marker"))
        self.assertEqual(tactical["space"]["camera"]["source"], "space map camera (project config, Space_Mode XML)")
        plain_camera = dict(plain["capture_identity"]["camera"])
        tactical_camera = dict(tactical["capture_identity"]["camera"])
        plain_eye = plain_camera.pop("position")
        tactical_eye = tactical_camera.pop("position")
        self.assertEqual(plain_camera, tactical_camera)
        target = plain_camera["target"]
        plain_offset = [eye - aim for eye, aim in zip(plain_eye, target)]
        self.assertAlmostEqual(math.hypot(*plain_offset), 1000.0, delta=0.05)
        for axis, offset in enumerate(plain_offset):
            self.assertAlmostEqual(tactical_eye[axis] - target[axis], offset * 1.2, delta=0.05)


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the FoC corpus run")
    def test_foc_reference_map_is_default_on_foc_install(self):
        pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
        entry = next(item for item in pinned["references"]
                     if item["role"] == "m1_reference" and item["checkpoint"]["kind"] == "space")
        with tempfile.TemporaryDirectory(prefix="eawr-space-foc-") as temporary:
            code, result, _ = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                        entry["logical_path"], pathlib.Path(temporary), "coruscant",
                                        ("--eawr-space-camera", fixture.CAMERA, *NONE))
            self.assertEqual(result["profile"], "foc")
            self.assertEqual(result["layers"], ["expansion", "base"])
            self.assertEqual(result["map"]["sha256"], entry["sha256"])
            self.assertEqual(result["status"], "space_blocked")
            self.assertNotEqual(code, 0)


    def test_synthetic_primary_sky_draws_each_surface_with_its_own_texture(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, _ = self._synthetic(directory, "baseline")
            self.assertEqual(code, 0, result["failure"])
            self.assertEqual(result["status"], "space_primary_sky_passed")
            self._check_identity(result)
            space = result["space"]
            self.assertEqual(space["load_status"], "loaded")
            self.assertEqual(space["material_status"], "compiled")
            self.assertEqual(space["submission_status"], "matched")
            self.assertEqual(space["pixel_evidence_status"], "verified")
            self.assertEqual(space["sky_object"]["declared_tag"], "Space_Model_Name")
            self.assertEqual(result["populate"]["declared_placements"], fixture.EXPECTED["declared_placements"])
            surfaces = space["surfaces"]
            self.assertEqual(len(surfaces), fixture.EXPECTED["surfaces"])
            self.assertEqual(len({surface["texture"]["logical_path"] for surface in surfaces}), 2)
            self.assertEqual(len({surface["renderer_asset"] for surface in surfaces}), 2)
            self.assertEqual(surfaces[0]["texture"]["source_origin"], "bottom_left")
            self.assertEqual(surfaces[1]["texture"]["format"], "bgra8")
            for surface, texture in zip(surfaces, ("eawr_space_a.tga", "eawr_space_b.dds")):
                self.assertEqual(surface["status"], "accepted")
                self.assertEqual(surface["adapter_id"], "eawr-space-primary-sky-v1")
                self.assertEqual(surface["original_shader"], fixture.QUALIFIED_SHADER)
                self.assertEqual((surface["technique"], surface["pass_name"]), ("", ""))
                self.assertEqual(surface["compiler"], "compiled")
                self.assertIs(surface["casts_shadows"], False)
                self.assertRegex(surface["texture"]["sha256"], r"^[0-9a-f]{64}$")
                pixels = surface["pixels"]
                self.assertEqual(pixels["status"], "verified")
                self.assertEqual(pixels["isolated_changed_outside"], 0)
                # Orientation and swizzle: each UV quadrant shows its own
                # texture quadrant's colour, not a mirrored or BGR one.
                for quadrant, expected in zip(pixels["uv_quadrants"], fixture.QUADRANTS[texture]):
                    self.assertGreater(quadrant["pixels"], 1000)
                    for measured, wanted in zip(quadrant["mean_rgb"], expected):
                        self.assertAlmostEqual(measured, wanted, delta=6, msg=texture)
            expected = [(surface["entity"], surface["renderer_asset"]) for surface in surfaces]
            observed = [(item["entity"], item["asset"]) for item in space["submissions"]["observed"]]
            self.assertEqual(observed, expected)
            self.assertEqual(space["lifecycle"]["resources_after_teardown"], 0)
            self.assertEqual(space["lifecycle"]["instances_after_teardown"], 0)
            self.assertEqual(space["evidence"]["changed_outside"], 0)

            # Repeatable: the same inputs capture the same pixels.
            code, again, _ = self._synthetic(directory, "again")
            self.assertEqual(code, 0)
            self.assertEqual(again["captures"]["configured"], result["captures"]["configured"])


    def test_rigid_parent_sky_moves_in_predeclared_regions_repeatably(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-rigid-") as temporary:
            directory = pathlib.Path(temporary)
            code, baseline, baseline_png = self._synthetic(directory, "baseline")
            self.assertEqual(code, 0, baseline["failure"])
            code, rigid, rigid_png = self._synthetic(directory, "rigid", "rigid")
            self.assertEqual(code, 0, rigid["failure"])
            self.assertEqual(rigid["status"], "space_primary_sky_passed")
            self._check_identity(rigid)
            self.assertEqual(rigid["space"]["pixel_evidence_status"], "verified")
            self.assertEqual(rigid["space"]["evidence"]["changed_outside"], 0)
            for surface, texture in zip(rigid["space"]["surfaces"],
                                        ("eawr_space_a.tga", "eawr_space_b.dds")):
                self.assertEqual(surface["status"], "accepted")
                self.assertEqual(surface["pixels"]["status"], "verified")
                for quadrant, expected in zip(surface["pixels"]["uv_quadrants"], fixture.QUADRANTS[texture]):
                    self.assertGreater(quadrant["pixels"], 1000)
                    for measured, wanted in zip(quadrant["mean_rgb"], expected):
                        self.assertAlmostEqual(measured, wanted, delta=6)
            width, height, before = decode_png(baseline_png.read_bytes())
            _, _, after = decode_png(rigid_png.read_bytes())
            masks = []
            for path in (baseline_png, rigid_png):
                for surface in range(2):
                    mw, mh, bits = read_pgm(path.with_suffix(f".mask-surface-{surface}.pgm"))
                    self.assertEqual((mw, mh), (width, height))
                    masks.append(bits)
            self.assertNotEqual(masks[0], masks[2], "SkyA must move")
            self.assertNotEqual(masks[1], masks[3], "SkyB must move")
            moved = outside = 0
            for y in range(height):
                for x in range(width):
                    if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                        continue
                    moved += 1
                    if not any(mask[y][x] for mask in masks):
                        outside += 1
            self.assertGreater(moved, 1000)
            self.assertEqual(outside, 0)
            self.assertEqual(rigid["space"]["lifecycle"]["resources_after_teardown"], 0)
            self.assertEqual(rigid["space"]["lifecycle"]["instances_after_teardown"], 0)
            code, repeat, _ = self._synthetic(directory, "rigid-repeat", "rigid")
            self.assertEqual(code, 0, repeat["failure"])
            self.assertEqual(repeat["captures"]["configured"], rigid["captures"]["configured"])
            self.assertEqual(repeat["space"]["lifecycle"]["resources_after_teardown"], 0)
            self.assertEqual(repeat["space"]["lifecycle"]["instances_after_teardown"], 0)


    def test_texture_swap_changes_only_that_surface(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-swap-") as temporary:
            directory = pathlib.Path(temporary)
            code, baseline, baseline_png = self._synthetic(directory, "baseline")
            self.assertEqual(code, 0, baseline["failure"])
            code, swapped, swapped_png = self._synthetic(directory, "swap", "swap")
            self.assertEqual(code, 0, swapped["failure"])
            first = baseline["space"]["surfaces"]
            second = swapped["space"]["surfaces"]
            self.assertEqual(first[0]["texture"]["sha256"], second[0]["texture"]["sha256"])
            self.assertNotEqual(first[1]["texture"]["sha256"], second[1]["texture"]["sha256"])
            self.assertEqual(first[0]["pixels"]["uv_quadrants"], second[0]["pixels"]["uv_quadrants"])
            for quadrant, expected in zip(second[1]["pixels"]["uv_quadrants"], fixture.SWAPPED_B):
                for measured, wanted in zip(quadrant["mean_rgb"], expected):
                    self.assertAlmostEqual(measured, wanted, delta=6)
            # Pixel attribution outside this process: every changed pixel lies
            # in surface 1's predeclared (dilated) mask.
            width, height, before = decode_png(baseline_png.read_bytes())
            _, _, after = decode_png(swapped_png.read_bytes())
            mask_width, mask_height, mask = read_pgm(baseline_png.with_suffix(".mask-surface-1.pgm"))
            _, _, other = read_pgm(baseline_png.with_suffix(".mask-surface-0.pgm"))
            self.assertEqual((width, height), (mask_width, mask_height))
            changed_inside = changed_outside = changed_other = 0
            for y in range(height):
                for x in range(width):
                    if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                        continue
                    if mask[y][x]:
                        changed_inside += 1
                    else:
                        changed_outside += 1
                    if other[y][x]:
                        changed_other += 1
            self.assertGreater(changed_inside, 10000)
            self.assertEqual(changed_outside, 0)
            self.assertEqual(changed_other, 0)


    def test_negative_controls_invalidate_their_evidence(self):
        camera = ("--eawr-space-camera", fixture.CAMERA)
        with tempfile.TemporaryDirectory(prefix="eawr-space-negative-") as temporary:
            directory = pathlib.Path(temporary)

            code, result, _ = self._synthetic(directory, "drop", extra=camera + ("--eawr-space-control", "drop-submission"))
            self._expect_failure(code, result, "failed", "removed submission")
            self.assertEqual(result["space"]["submission_status"], "mismatch")
            self.assertEqual(result["space"]["surfaces"][1]["pixels"]["status"], "not_submitted")
            self.assertEqual(result["space"]["surfaces"][0]["pixels"]["status"], "verified")

            code, result, _ = self._synthetic(directory, "broken", extra=camera + ("--eawr-space-control", "broken-shader"))
            self._expect_failure(code, result, "failed", "broken shader")
            self.assertEqual(result["space"]["material_status"], "compile_failed")
            self.assertEqual(result["space"]["surfaces"][1]["compiler"], "rejected")
            self.assertIn("EAWR-RENDER-0007", result["space"]["surfaces"][1]["upload_failure"])
            lifecycle = result["space"]["lifecycle"]
            self.assertTrue(lifecycle["partial_failure"])
            self.assertEqual(lifecycle["resources_after_partial_failure"], 0)

            code, result, _ = self._synthetic(directory, "offcam", extra=("--eawr-space-camera", fixture.OFF_CAMERA, *NONE))
            self._expect_failure(code, result, "failed", "frustum-out camera")
            self.assertEqual({s["pixels"]["status"] for s in result["space"]["surfaces"]}, {"not_projected"})
            self.assertEqual(result["space"]["submission_status"], "matched",
                             "submission is counted separately from visible pixels")

            code, result, _ = self._synthetic(directory, "occfull", extra=camera + ("--eawr-space-control", "occluder-full"))
            self._expect_failure(code, result, "failed", "fully occluded surface")
            self.assertEqual(result["space"]["surfaces"][0]["pixels"]["status"], "occluded")
            self.assertEqual(result["space"]["evidence"]["occlusion"]["status"], "verified")

            code, result, _ = self._synthetic(directory, "missing", "missing_texture")
            self._expect_failure(code, result, "space_blocked", "missing texture must not grey-pass")
            self.assertEqual(result["space"]["surfaces"][1]["status"], "texture_not_in_vfs")
            self.assertEqual(result["space"]["material_status"], "not_attempted")

            code, result, _ = self._synthetic(directory, "unqualified", "unqualified_shader")
            self._expect_failure(code, result, "space_blocked", "unreviewed shader identity")
            self.assertEqual(result["space"]["surfaces"][1]["status"], "shader_not_qualified")

            code, result, _ = self._synthetic(directory, "nocamera", extra=NONE)
            self._expect_failure(code, result, "failed", "no fixed camera")
            self.assertEqual(result["space"]["camera"]["status"], "malformed")

            code, result, _ = self._synthetic(directory, "collinear", extra=(
                "--eawr-space-camera", "0,0,0,0,-1,0,0,1,0,60,1,5000", *NONE))
            self._expect_failure(code, result, "failed", "collinear up vector")
            self.assertEqual(result["space"]["camera"]["status"], "up_collinear")

            code, result, _ = self._synthetic(directory, "lit", extra=camera + ("--eawr-lighting", "sh"))
            self._expect_failure(code, result, "failed", "lighting is not composed for space")
            self.assertIn("composes no lighting", result["failure"])

    def test_unsupported_primary_sky_continues(self):
        with tempfile.TemporaryDirectory(prefix="eawr-unsupported-sky-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, _ = self._synthetic(directory, "populate", extra=(
                "--eawr-space-camera", fixture.CAMERA, "--eawr-populate"))
            self.assertEqual(code, 0, result["failure"])
            self.assertEqual(result["space"]["slice"], "E-space-environment-v1")
            self.assertTrue(all(item["status"] == "not_drawn" for item in result["space"]["surfaces"]))
            self.assertIn("primary sky drew no surface; continuing without a primary sky",
                          result["space"]["not_rendered"])


    def test_missing_tactical_cameras_fails_before_sky(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-missing-tactical-camera-") as temporary:
            directory = pathlib.Path(temporary)
            root = fixture.write_fixture_root(directory / "fixture")
            (root / "GameData/Data/XML/TacticalCameras.xml").unlink()
            code, result, _ = self._run(
                root, fixture.MAP_LOGICAL_PATH, directory, "missing-tactical-camera",
                ("--eawr-space-camera", fixture.CAMERA, "--eawr-populate"))
            self._expect_failure(code, result, "failed", "missing tactical camera XML")
            self.assertEqual(result["space"]["slice"], "E-space-environment-v1")
            self.assertIn("Space_Mode camera: EAWR-VFS-0002", result["failure"])
            self.assertIn("data/xml/tacticalcameras.xml", result["failure"])
            self.assertIn("asset is not present in the mounted view", result["failure"])
            self.assertNotIn("primary sky drew no surface", result["failure"])


    def test_nonfinite_camera_fails_with_strict_json(self):
        # M1: NaN, infinity and a float overflow are refused before any upload,
        # and the failure report stays RFC 8259 JSON (strict_json in _run).
        cameras = {
            "nan": "nan,0,0,0,0,-1,0,1,0,60,1,5000",
            "inf": "0,0,0,0,0,-1,0,1,0,inf,1,5000",
            "neginf": "0,0,0,0,0,-1,0,1,0,60,1,-inf",
            "overflow": "0,0,0,0,0,-1,0,1e999,0,60,1,5000",
        }
        with tempfile.TemporaryDirectory(prefix="eawr-space-camera-") as temporary:
            directory = pathlib.Path(temporary)
            for name, text in cameras.items():
                code, result, _ = self._synthetic(directory, name, extra=("--eawr-space-camera", text, *NONE))
                self._expect_failure(code, result, "failed", name)
                space = result["space"]
                self.assertEqual(space["camera"]["status"], "nonfinite", name)
                self.assertEqual(space["camera"]["input"], text, name)
                self.assertEqual(space["material_status"], "not_attempted", name)
                self.assertIsNone(result["capture_identity"]["camera"], name)


    def test_unwritable_requested_artifacts_fail_and_release(self):
        # M2: every requested capture and mask write is checked; a failure
        # names the artifact, exits non-zero and still releases the uploads.
        with tempfile.TemporaryDirectory(prefix="eawr-space-artifacts-") as temporary:
            directory = pathlib.Path(temporary)
            blocker = directory / "blocker"
            blocker.write_bytes(b"a regular file, not a directory")
            cases = {
                # Parent of the configured capture is a regular file.
                "parent": (blocker / "capture.png", "capture.png"),
                # A directory already occupies a phase capture's path.
                "phase": (directory / "phase.png", "phase.sky_disabled.png"),
                # A directory already occupies surface 0's exported mask path.
                "mask": (directory / "mask.png", "mask.mask-surface-0.pgm"),
            }
            (directory / "phase.sky_disabled.png").mkdir()
            (directory / "mask.mask-surface-0.pgm").mkdir()
            for name, (capture, artifact) in cases.items():
                code, result, _ = self._synthetic(directory, name, capture=capture)
                self._expect_failure(code, result, "failed", name)
                self.assertIn(artifact, result["failure"], name)
                failed = result["space"]["artifacts"]["failed"]
                self.assertTrue(any(artifact in item for item in failed), (name, failed))
                self.assertNotEqual(result["space"]["artifacts"]["status"], "written", name)
                lifecycle = result["space"]["lifecycle"]
                self.assertEqual(lifecycle["resources_after_teardown"], 0, name)
                self.assertEqual(lifecycle["instances_after_teardown"], 0, name)


    def test_hidden_bone_visibility_fails_closed(self):
        # M3: an identity chain carrying a hidden bone is not silently dropped
        # into an unconditionally visible boneless upload.
        with tempfile.TemporaryDirectory(prefix="eawr-space-bones-") as temporary:
            directory = pathlib.Path(temporary)
            for variant, bone in (("hidden_bone", "Root"), ("hidden_ancestor", "Root")):
                code, result, _ = self._synthetic(directory, variant, variant)
                self._expect_failure(code, result, "space_blocked", variant)
                space = result["space"]
                self.assertEqual(space["plan_status"], "surface_rejected", variant)
                self.assertEqual(space["material_status"], "not_attempted", variant)
                for surface in space["surfaces"]:
                    self.assertEqual(surface["status"], "bone_visibility_unsupported", variant)
                    self.assertIn(f"'{bone}'", surface["detail"], variant)
                    self.assertIn("hidden", surface["detail"], variant)
                    self.assertEqual(surface["renderer_asset"], 0, variant)


    def test_occluder_and_lifecycle_controls_pass(self):
        camera = ("--eawr-space-camera", fixture.CAMERA)
        with tempfile.TemporaryDirectory(prefix="eawr-space-controls-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, _ = self._synthetic(directory, "occluder", extra=camera + ("--eawr-space-control", "occluder"))
            self.assertEqual(code, 0, result["failure"])
            occlusion = result["space"]["evidence"]["occlusion"]
            self.assertEqual(occlusion["status"], "verified")
            self.assertGreater(occlusion["occluder_pixels"], 1000)
            self.assertEqual(occlusion["occluder_changed_by_sky"], 0)
            self.assertEqual(result["space"]["lifecycle"]["resources_after_teardown"], 0)

            # Live lifecycle: each cycle submits and draws the composed sky, so
            # instances exist when its assets are released and re-uploaded.
            code, baseline, _ = self._synthetic(directory, "reload-baseline")
            self.assertEqual(code, 0, baseline["failure"])
            code, result, _ = self._synthetic(directory, "reload", extra=camera + ("--eawr-space-control", "reload-cycle"))
            self.assertEqual(code, 0, result["failure"])
            lifecycle = result["space"]["lifecycle"]
            self.assertEqual(lifecycle["reload_cycles"], 3)
            self.assertEqual(lifecycle["mode"], "live")
            self.assertEqual(lifecycle["instances_before_release"], [2, 2, 2])
            self.assertEqual(lifecycle["resources_before_release"], [2, 2, 2])
            self.assertEqual(lifecycle["instances_after_release"], [0, 0, 0])
            self.assertEqual(lifecycle["resources_after_release"], [0, 0, 0])
            self.assertEqual(lifecycle["resources_after_reupload"], [2, 2, 2])
            self.assertEqual(lifecycle["resources_after_teardown"], 0)
            self.assertEqual(lifecycle["instances_after_teardown"], 0)
            self.assertEqual(result["captures"]["configured"], baseline["captures"]["configured"],
                             "the composition after three live cycles draws the same pixels")


    def test_lit_foreground_excludes_the_sky_from_shadows(self):
        # Design gate: a controlled lit receiver lies where surface 0 would
        # shadow it under one fixed directional light. The positive control
        # (a depth-writing, two-winding copy of the sky surfaces with casting
        # enabled) must darken the receiver; the same caster with the cast
        # flag off, and the shipped sky, must leave it unchanged.
        camera = ("--eawr-space-camera", fixture.CAMERA)
        with tempfile.TemporaryDirectory(prefix="eawr-space-shadow-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for control in ("sky-shadow-caster-cast", "sky-shadow-caster", "sky-shadow-cast", "sky-shadow"):
                runs[control] = self._synthetic(directory, control, extra=camera + ("--eawr-space-control", control))

            code, positive, _ = runs["sky-shadow-caster-cast"]
            self._expect_failure(code, positive, "failed", "a casting surface must shadow the lit receiver")
            shadows = positive["space"]["shadows"]
            self.assertIs(shadows["sky_cast_flag"], True)
            self.assertEqual(shadows["sky_material"], "depth_writing_caster_control")
            self.assertEqual(shadows["lit_foreground_control"], "shadowed_by_sky")
            receiver_pixels = shadows["receiver_pixels"]
            self.assertGreater(receiver_pixels, 1000)
            self.assertGreater(shadows["receiver_changed_by_sky"] * 10, receiver_pixels * 9)
            self.assertIn("shadowed the lit foreground receiver", positive["failure"])

            expected = {
                # control: (cast flag, material)
                "sky-shadow-caster": (False, "depth_writing_caster_control"),
                "sky-shadow-cast": (True, "shipped_adapter"),
                "sky-shadow": (False, "shipped_adapter"),
            }
            for control, (flag, material) in expected.items():
                code, result, _ = runs[control]
                self.assertEqual(code, 0, (control, result["failure"]))
                self.assertEqual(result["status"], "space_primary_sky_passed", control)
                shadows = result["space"]["shadows"]
                self.assertIs(shadows["sky_cast_flag"], flag, control)
                self.assertEqual(shadows["sky_material"], material, control)
                self.assertEqual(shadows["lit_foreground_control"], "unchanged", control)
                self.assertEqual(shadows["receiver_pixels"], receiver_pixels, control)
                self.assertEqual(shadows["receiver_changed_by_sky"], 0, control)
                self.assertTrue(result["lighting"]["composed"], control)
                self.assertEqual(result["space"]["submissions"]["expected"][-1]["role"], "lit_receiver_control")
                for surface in result["space"]["surfaces"]:
                    self.assertEqual(surface["pixels"]["status"], "verified", control)
                    self.assertIs(surface["casts_shadows"], flag, control)


    def test_land_path_refuses_space_options(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-land-") as temporary:
            directory = pathlib.Path(temporary)
            root = scene_fixture.write_fixture_root(directory / "land")
            code, result, _ = self._run(root, scene_fixture.MAP_LOGICAL_PATH, directory, "land",
                                        ("--eawr-space-camera", fixture.CAMERA))
            self.assertNotEqual(code, 0)
            self.assertEqual(result["map"]["kind"], "land")
            self.assertIn("kind-2", result["failure"])


    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the read-only pinned space map run")
    def test_pinned_space_reference_map_passes_or_names_its_blocker(self):
        pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
        entry = next(item for item in pinned["references"] if item["logical_path"] == ALDERAAN_SPACE)
        self.assertEqual(entry["role"], "regression_fixture")
        self.assertEqual(entry["checkpoint"]["kind"], "space")
        with tempfile.TemporaryDirectory(prefix="eawr-space-corpus-") as temporary:
            directory = pathlib.Path(temporary)
            # A camera at the source origin is only an input for this probe; no
            # corpus camera is frozen, so a pass here is not an accepted view.
            code, result, _ = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), entry["logical_path"],
                                        directory, "alderaan", ("--eawr-profile", "eaw", "--eawr-space-camera", fixture.CAMERA, *NONE))
            self.assertEqual(result["map"]["sha256"], entry["sha256"])
            self.assertEqual(result["map"]["kind"], "space")
            self.assertIn(result["status"], SPACE_STATUSES, result["failure"])
            space = result["space"]
            self.assertFalse(space["environment_complete"])
            self.assertEqual(space["environment"]["count"], entry["checkpoint"]["environments"])
            # Pinned current classification (P1-06-space-primary-sky.md, with
            # the MeshGloss route of P1-06-meshgloss-sky-route.md): the
            # declared primary sky resolves to a two-surface model whose star
            # surface is accepted and whose sun billboard is rejected with
            # these named causes, so nothing is drawn. An earlier
            # catalog/model blocker, a different surface set, or a pass all
            # fail here; a pass must be accepted deliberately, not by default.
            self.assertEqual(result["status"], "space_blocked", result["failure"])
            self.assertNotEqual(code, 0)
            self.assertEqual(space["plan_status"], "surface_rejected")
            self.assertEqual(space["load_status"], "blocked")
            self.assertEqual(space["material_status"], "not_attempted")
            self.assertEqual(space["environment"]["primary_sky"], ALDERAAN_BLOCKER["primary_sky"])
            self.assertEqual(space["sky_object"]["status"], "ready")
            self.assertEqual(space["sky_object"]["declared_tag"], "Space_Model_Name")
            self.assertEqual(space["model"]["logical_path"], ALDERAAN_BLOCKER["model"])
            self.assertEqual(space["model"]["sha256"], ALDERAAN_BLOCKER["model_sha256"])
            surfaces = space["surfaces"]
            self.assertEqual(len(surfaces), len(ALDERAAN_BLOCKER["surfaces"]))
            for surface, pinned in zip(surfaces, ALDERAAN_BLOCKER["surfaces"]):
                name, shader, texture, first, required = pinned
                self.assertEqual(surface["mesh_name"], name)
                self.assertEqual(surface["original_shader"], shader)
                self.assertEqual(surface["texture"]["logical_path"], texture)
                self.assertEqual(surface["status"], first, surface)
                self.assertTrue(set(required) <= set(surface["causes"]), surface)
                if first == "accepted":
                    self.assertEqual(surface["causes"], [], surface)
                    self.assertEqual(surface["material"]["route"], "meshgloss", surface)
                    self.assertEqual(surface["adapter_id"], "eawr-space-sky-meshgloss-v1", surface)
                    self.assertEqual(surface["unconsumed_parameters"], ["Shininess"], surface)
                self.assertEqual(surface["renderer_asset"], 0)
