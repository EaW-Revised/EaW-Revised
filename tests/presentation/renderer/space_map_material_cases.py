"""Cases for test_space_map_mode; collected by its legacy facade."""

from space_map_test_support import (
    ROOT, decode_png, fixture, json,
    math, os, pathlib, scene_fixture,
    strict_json, struct, subprocess, sys,
    tempfile, unittest,
)


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
