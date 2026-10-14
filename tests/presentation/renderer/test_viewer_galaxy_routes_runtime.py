"""Textured trade routes, independent relation slots, sparks and travelling lines."""
import json
import math
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class GalaxyRoutesRuntime(unittest.TestCase):
    def capture(self, focused=False, paused=False, style=None, fixture=None, seat=1):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-galaxy-routes-"))
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
                str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", "synthetic",
                "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes" if paused else "no",
                "--eawr-report", str(directory / "report.json"), "--eawr-capture", str(directory / "capture.png")]
        if focused:
            args += ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "650"]
        if fixture:
            args += ["--eawr-galactic-hud-fixture", fixture, "--eawr-galactic-seat", str(seat)]
        if style:
            width, colour = style
            xml = directory / "mod" / "Data" / "XML" / "TradeRouteLines.xml"
            xml.parent.mkdir(parents=True)
            (xml.parent.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            slots = "".join(
                f'<Settings_For_Faction Name="{relation}"><Width>{width}</Width>'
                f'<Color_Zoomed_In>{colour}</Color_Zoomed_In><Color_Zoomed_Out>{colour}</Color_Zoomed_Out>'
                '<Render_Mode>ADDITIVE</Render_Mode></Settings_For_Faction>'
                for relation in ("Neutral", "Enemy", "Ally", "Good", "Evil", "Corrupt"))
            xml.write_text(f'<TradeRouteLines><TradeRouteLine Name="Default">{slots}</TradeRouteLine></TradeRouteLines>', encoding="utf-8")
            args += ["--eawr-mod-root", str(directory / "mod")]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        self.assertTrue((directory / "report.json").is_file(), result.stdout + result.stderr)
        data = json.loads((directory / "report.json").read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["status"], "captured")
        return data, directory / "capture.png"

    def test_relation_slots_textures_sparks_and_fleet_lines(self):
        data, _ = self.capture()
        self.assertEqual(data["route_geometry"]["strips"], 12)
        self.assertEqual(data["route_geometry"]["sparks"], 24)
        # FT-12b reads actual membership: the synthetic owner seeds departure
        # orbits until locomotion publishes travel, rather than inventing transit.
        self.assertEqual(data["route_geometry"]["fleet_lines"], 0)
        self.assertTrue(all(not fleet["transit"] for fleet in data["fleets"]))
        routes = {route["id"]: route for route in data["routes"]}
        self.assertEqual(routes[201]["line_type"], "good")
        self.assertEqual(routes[210]["line_type"], "evil")
        self.assertEqual(routes[203]["line_type"], "neutral")
        for route in routes.values():
            self.assertEqual(route["width"], 3)
            self.assertTrue(route["texture"].lower().endswith("w_galaxy_line_alpha.tga"))
            self.assertTrue(route["spark_texture"].lower().endswith("w_galaxy_dot.tga"))
        fleet = next(model for model in data["models"] if model["logical_path"].lower().endswith("w_planet_fleet.alo"))
        self.assertEqual(fleet["status"], "drawn", fleet)

    def test_focused_zoom_removes_sparks_and_uses_zoomed_in_colours(self):
        data, _ = self.capture(focused=True)
        self.assertEqual(data["route_geometry"]["sparks"], 0)
        self.assertTrue(all(route["zoom"] == 1 for route in data["routes"]))

    def test_pause_freezes_route_animation(self):
        data, _ = self.capture(paused=True)
        self.assertEqual(data["animation_updates"], 1)
        self.assertEqual(data["route_geometry"]["animation_updates"], 1)

    def test_fleet_models_consume_their_owners_colour(self):
        checked = set()
        for seat, faction in ((3, "Empire"), (4, "Underworld")):
            data, capture = self.capture(paused=True, fixture="fleet-transit", seat=seat)
            from space_map_test_support import decode_png
            rows = decode_png(capture.read_bytes())[2]
            camera = data["camera"]
            def dot(a, b):
                return sum(x * y for x, y in zip(a, b))
            def unit(a):
                length = math.sqrt(dot(a, a))
                return [x / length for x in a]
            forward = unit([a - b for a, b in zip(camera["target"], camera["eye"])])
            right = unit([-forward[2], 0, forward[0]])
            up = [right[1] * forward[2] - right[2] * forward[1],
                  right[2] * forward[0] - right[0] * forward[2],
                  right[0] * forward[1] - right[1] * forward[0]]
            focal = camera["height"] / (2 * math.tan(math.radians(camera["vertical_fov"]) / 2))
            for fleet in data["fleets"]:
                # WGP-07/16: parked fleets use HUD icons; travelling fleets draw
                # the owner-colourised world model. Prove the model is present.
                if not fleet["transit"] or fleet["faction"] != faction:
                    continue
                self.assertGreater(fleet["world_model_instances"], 0, fleet)
                source = fleet["position"]
                offset = [a - b for a, b in zip([source[0], source[2], -source[1]], camera["eye"])]
                depth = dot(offset, forward)
                x = round(camera["width"] / 2 + focal * dot(offset, right) / depth)
                y = round(camera["height"] / 2 - focal * dot(offset, up) / depth)
                self.assertTrue(8 <= x < camera["width"] - 8 and 8 <= y < camera["height"] - 8,
                                f"fleet {fleet['fleet']} colour probe lies outside the viewport: {(x, y)}")
                pixels = [rows[py][px] for py in range(max(0, y - 8), min(camera["height"], y + 9))
                          for px in range(max(0, x - 8), min(camera["width"], x + 9))]
                if fleet["faction"] == "Empire":
                    coloured = sum(b > 35 and b > r * 1.4 and b > g * 1.15 for r, g, b, *_ in pixels)
                else:
                    coloured = sum(g > 35 and r > b * 2 and g > b * 2 and r < g * 2 for r, g, b, *_ in pixels)
                self.assertGreaterEqual(coloured, 3, f"fleet {fleet['fleet']} ignores {fleet['faction']} colour")
                checked.add(fleet["faction"])
        self.assertEqual(checked, {"Empire", "Underworld"})

    def test_effective_style_width_and_colour_change_pixels(self):
        from space_map_test_support import decode_png
        _, thin = self.capture(paused=True, style=(1, "0,0,255,255"))
        _, wide = self.capture(paused=True, style=(9, "0,0,255,255"))
        _, green = self.capture(paused=True, style=(9, "0,255,0,255"))
        rows = [decode_png(path.read_bytes())[2] for path in (thin, wide, green)]
        pixels = [[pixel for row in image for pixel in row] for image in rows]
        self.assertGreater(sum(a != b for a, b in zip(pixels[0], pixels[1])), 100)
        shifted = sum(b[1] > a[1] + 15 and a[2] > b[2] + 15
                      for a, b in zip(pixels[1], pixels[2]))
        self.assertGreater(shifted, 40)


if __name__ == "__main__":
    unittest.main()
