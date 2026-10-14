"""S13 shared campaign summary: sourced figures/visibility and native return."""
import unittest
from tests.presentation.renderer import test_viewer_galactic_battle_dialog_runtime as runtime_support
from tests.presentation.renderer import test_viewer_planet_hud_runtime as support

@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT,"requires GPU viewer and installed FoC")
class CampaignSummaryAdapters(unittest.TestCase):
    capture = runtime_support.GalacticBattleDialogRuntime.capture
    control = staticmethod(runtime_support.GalacticBattleDialogRuntime.control)
    click = runtime_support.GalacticBattleDialogRuntime.click

    def test_efficiency_bounty_and_native_return(self):
        for fixture,pair,caption in [
            ("battle-summary-zero",[0,100],None),
            ("battle-summary-half",[13,88],None),
            ("battle-summary-bounty",[75,25],"Collected bounty: 42"),
            ("battle-summary-corruption",[75,25],"Collected bounty: 0"),
        ]:
            with self.subTest(fixture=fixture):
                data,capture = self.capture(fixture)
                self.assertTrue(capture.is_file())
                self.assertEqual(data["battle_dialog"]["efficiency"],pair)
                bounty = self.control(data,"IDC_STATIC_BOUNTY")
                self.assertEqual(bounty["visible"],caption is not None)
                if caption: self.assertEqual(bounty["text"],caption)
                self.assertEqual(data["battle_dialog"]["summary_refreshes"],1)
                button = self.control(data,"IDC_CONTINUE_BUTTON")
                self.assertEqual(button["text"].strip(),"Return to Galactic")
                returned = self.click(fixture,button)
                self.assertEqual(returned["battle_dialog"]["phase"],"hidden")
                self.assertEqual(returned["battle_dialog"]["choices"],1)

    def test_auto_candidate_does_not_show_invasion(self):
        data,_ = self.capture("battle-summary-auto-offer")
        self.assertTrue(data["battle_dialog"]["auto_resolved"])
        self.assertFalse(self.control(data,"IDC_LAND_INVADE_BUTTON")["visible"])

if __name__ == "__main__": unittest.main()

