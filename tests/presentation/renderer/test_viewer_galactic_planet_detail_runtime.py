"""GPD-02/03: the advanced planetary map draws the land map's picture, and its pads sit on that picture.
GPD-29: the focused view draws the landing slot only while a fleet in orbit can land."""
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_fleet_runtime as fleets
    from . import test_viewer_galactic_land_tray_runtime as tray
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_fleet_runtime as fleets
    import test_viewer_galactic_land_tray_runtime as tray

# A planet with an installed land map in the stock Equal Footing Empire campaign (planet ids follow the campaign's planet list).
CAMPAIGN = "Sandbox_Equal_Footing_Empire"
CORUSCANT = "15"


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPlanetDetailRuntime(unittest.TestCase):
    def capture_map(self):
        return support.PlanetHudRuntime.capture(self, CAMPAIGN,
            "--eawr-galactic-focus", CORUSCANT, "--eawr-galactic-flight-ms", "750",
            "--eawr-galactic-advanced-map", "yes", "--eawr-galactic-dismiss-story", "yes")

    def test_command_bar_is_hidden_only_while_the_map_is_open(self):
        # GPD-26: the ordinary command bar is not drawn under the opened map. The bar's visibility is
        # recomputed from the camera's advanced-map flag on every HUD update, so any exit (back, wheel,
        # battle start and return, load) restores it.
        opened, _ = self.capture_map()
        self.assertFalse(opened["command_bar"]["shown"], "the command bar is drawn under the opened map")
        closed, _ = support.PlanetHudRuntime.capture(self, CAMPAIGN,
            "--eawr-galactic-focus", CORUSCANT, "--eawr-galactic-flight-ms", "750", "--eawr-galactic-dismiss-story", "yes")
        self.assertTrue(closed["command_bar"]["shown"], "the command bar is missing in the focused view")
        overview, _ = support.PlanetHudRuntime.capture(self, CAMPAIGN, "--eawr-galactic-dismiss-story", "yes")
        self.assertTrue(overview["command_bar"]["shown"], "the command bar is missing in the galaxy view")

    def test_day_progress_bar_follows_the_day_text(self):
        # GPD-44 (legacy #5628): the opened map draws the command bar's ten day pips from the shell's day clock bone,
        # on the day text's row, lit up to the current one. The retail frame (1280x720 client, Coruscant, day 1) has
        # the green bar starting at about x 271 and spanning about y 509-520, read off the frame by eye, hence the tolerance.
        data, _ = self.capture_map()
        pips = data["radar_hud"]["day_pips"]
        self.assertEqual(len(pips), 10, pips)
        current = data["command_bar"]["current_pip"]
        self.assertIsNotNone(current, "the fiscal clock is unavailable")
        self.assertEqual([row["lit"] for row in pips], [index <= current for index in range(10)], pips)
        rows = {round(row["rect"][1], 1) for row in pips}
        self.assertEqual(len(rows), 1, "the pips share one row: %s" % pips)
        xs = [row["rect"][0] for row in pips]
        pitch = [b - a for a, b in zip(xs, xs[1:])]
        self.assertTrue(all(abs(step - pitch[0]) < 0.6 and step > 0 for step in pitch), "even spacing: %s" % pips)
        x, y, width, height = pips[0]["rect"]
        self.assertAlmostEqual(x, 271, delta=8, msg="the bar starts after the day text, as in the retail frame: %s" % pips)
        self.assertAlmostEqual(y + height / 2, 514.5, delta=8, msg="the bar sits on the day text row: %s" % pips)

    def test_nothing_of_the_galaxy_shows_beside_the_opened_map(self):
        # GPD-30: the opened map is a full-screen shell with nothing of the galaxy behind it, and the galaxy scene
        # is not rendered while it is open; closing the map brings both back.
        opened, _ = self.capture_map()
        black = opened["ground_hud"]["detail"]["black"]
        self.assertTrue(black["visible"], "the galaxy shows beside the opened map")
        x, y, width, height = black["rect"]
        self.assertEqual((x, y, width, height), (0, 0, 1280, 720), "the black rect does not cover the window")
        self.assertFalse(black["galaxy_updating"], "the galaxy scene is still rendered under the opened map")
        focused, _ = support.PlanetHudRuntime.capture(self, CAMPAIGN,
            "--eawr-galactic-focus", CORUSCANT, "--eawr-galactic-flight-ms", "750", "--eawr-galactic-dismiss-story", "yes")
        black = focused["ground_hud"]["detail"]["black"]
        self.assertFalse(black["visible"], "the focused view is covered in black")
        self.assertTrue(black["galaxy_updating"], "the galaxy scene is not rendered in the focused view")

    def test_closing_the_map_brings_the_galaxy_back(self):
        # The same map opened and then closed by a wheel step (the land-tray driver, which holds a tray unit while
        # closing): the black rect is gone and the galaxy viewports render again.
        data, _ = tray.GalacticLandTrayRuntime.run_drag(self, close=True)
        self.assertFalse(data["radar_hud"]["advanced"], "the wheel step did not close the advanced map")
        black = data["ground_hud"]["detail"]["black"]
        self.assertFalse(black["visible"], "the black rect stayed after the map closed: %s" % black)
        self.assertTrue(black["galaxy_updating"], "the galaxy scene did not resume after the map closed: %s" % black)

    def test_back_button_sits_below_the_map(self):
        # GPD-31: the opened map has a BACK button under the map window.
        data, _ = self.capture_map()
        detail = data["ground_hud"]["detail"]
        self.assertTrue(detail["back"]["visible"], "the BACK button is missing")
        bx, by, bw, bh = detail["back"]["rect"]
        mx, my, mw, mh = detail["preview"]["rect"]
        self.assertGreater(bw, 0)
        self.assertGreater(bh, 0)
        self.assertGreaterEqual(by, my + mh - 1, "the BACK button overlaps the map window")
        self.assertLessEqual(by + bh, 720 + 1, "the BACK button is below the screen")

    def test_pads_sit_on_the_land_map_picture(self):
        data, capture = self.capture_map()
        detail = data["ground_hud"]["detail"]
        self.assertTrue(detail["open"], detail)
        preview = detail["preview"]
        self.assertTrue(preview["visible"], "the land map picture is missing behind the pads")
        x, y, width, height = preview["rect"]
        self.assertGreater(width, 0)
        self.assertGreater(height, 0)
        self.assertTrue(detail["pads"], "no pad is drawn on an owned developed planet")
        for pad in detail["pads"]:
            px, py, pw, ph = pad["rect"]
            centre = (px + pw / 2, py + ph / 2)
            self.assertTrue(x <= centre[0] <= x + width and y <= centre[1] <= y + height,
                "pad %s%d centre %s is outside the map picture %s" % (pad["kind"], pad["slot"], centre, preview["rect"]))
        self.assertTrue(capture.is_file())

    def test_only_base_pads_and_occupied_specific_pads_are_drawn(self):
        # GPD-41 (legacy #5628): the retail frames draw the in-base and out-base pads (free or occupied) and a specific
        # pad only while a structure stands on it; free specific pads are not drawn.
        data, _ = self.capture_map()
        detail = data["ground_hud"]["detail"]
        builds = [row for row in detail["markers"] if row["group"] == "build"]
        specific = [index for index, row in enumerate(builds) if row["kind"] == 3]
        self.assertTrue(specific, "the fixture planet has no specific marker, the case proves nothing")
        base = [row for row in builds if row["kind"] != 3]
        drawn = {pad["slot"]: pad for pad in detail["pads"] if pad["kind"] == "build"}
        for slot in range(len(builds)):
            if builds[slot]["kind"] == 3 and slot in drawn:
                self.assertTrue(drawn[slot]["icon"], "free specific pad %d is drawn" % slot)
        self.assertGreaterEqual(len(drawn), len(base), "a base pad is missing")
        # GPD-43 (legacy #5628, #7168): an occupied pad shows its occupant's icon; a free pad shows none.
        occupied = [pad for pad in detail["pads"] if pad["object"]]
        self.assertTrue(occupied, "no pad is occupied, the fixture proves nothing about the icon")
        for pad in occupied:
            self.assertTrue(pad["icon"], "occupied %s pad %d (type %d, art %r) shows no icon"
                % (pad["kind"], pad["slot"], pad["type"], pad["art_name"]))
        for pad in detail["pads"]:
            if not pad["object"]:
                self.assertFalse(pad["icon"], "free %s pad %d shows an icon" % (pad["kind"], pad["slot"]))
        self.assertLess(len(drawn), len(builds), "no free specific pad was hidden: %d of %d" % (len(drawn), len(builds)))

    def test_landing_slot_follows_whether_a_fleet_in_orbit_can_land(self):
        # GPD-29. The same accessible focused planet, with and without a fleet that can land. Without the
        # eligibility rule the slot is drawn in both; an always-hidden slot fails the first case.
        landing = self.slot_controls("ground-contents")
        self.assertTrue(landing["Pad_00"]["visible"], "the focused ground pads are not shown")
        self.assertTrue(landing["Slot_00"]["visible"], "the landing slot is missing while a fleet can land")
        idle = self.slot_controls("ground-no-lander")
        self.assertTrue(idle["Pad_00"]["visible"], "the focused ground pads are not shown")
        self.assertFalse(idle["Slot_00"]["visible"], "the landing slot is drawn while no fleet can land")

    def slot_controls(self, fixture):
        data, _ = fleets.GalacticFleetRuntime.capture(self, fixture, True)
        return {row["name"]: row for row in data["ground_hud"]["controls"]}


if __name__ == "__main__":
    unittest.main()
