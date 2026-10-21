"""Text evidence distinguishes missing anchors from renamed downstream fields."""
import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/inventory'))
import tag_applied_verify as verify
import tag_registry_build as builder
import tag_registry as registry


class AppliedVerifyTests(unittest.TestCase):
    def test_reviewed_override_preserves_writer_and_downstream_consumer(self):
        instance = builder.Builder({}, {}, None, {}, set(), None, {}, {})
        row = registry.UniverseRow('Planet', 'Land_Tactical_Map', 'element')
        result = instance.decide(row)
        self.assertEqual([target['code'] for target in result['applied']], [
            'src/campaign/auto_resolve_live.cpp#record.map_identity',
            'src/platform/galactic/live_game_session.cpp#from_plan'])
        self.assertEqual(result['basis'], 'reviewed')

    def test_literal_and_anchor_misses_remain_separate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'consumer.cpp').write_text(
                '// "Unused" is only a comment\nvoid consume() { read("Actual"); }\n', encoding='utf-8')
            rows = [{'status': 'applied', 'classes': ['SpaceUnit'], 'area': 'combat', 'tag': tag,
                     'applied': [{'code': code}]} for tag, code in (
                         ('Nested/Actual', 'consumer.cpp#consume'),
                         ('Unused', 'consumer.cpp#consume'),
                         ('Actual', 'consumer.cpp#absent'),
                         ('Actual', 'missing.cpp#consume'))]
            targets = [r['targets'][0] for r in verify.verify(rows, root)]
            self.assertTrue(targets[0]['tag_literal'])
            self.assertTrue(targets[0]['identifier_exists'])
            self.assertFalse(targets[1]['tag_literal'])
            self.assertTrue(targets[1]['identifier_exists'])
            self.assertFalse(targets[2]['identifier_exists'])
            self.assertFalse(targets[3]['file_exists'])

    def test_ticket_successors_preserve_gap_status_and_named_plan_owner(self):
        row = {'status': 'partial', 'ticket': 1120, 'missing_types': ['Land', 'Space']}
        builder.refresh_audit_ticket(row, 'AITemplates', '*/Turn_On/Plans/Name')
        self.assertEqual(row, {'status': 'partial', 'ticket': 2585, 'missing_types': ['Land', 'Space']})
        applied = {'status': 'applied', 'ticket': 1114}
        builder.refresh_audit_ticket(applied, 'Planet', 'Political_Control')
        self.assertNotIn('ticket', applied)

    def test_remaining_modes_get_the_explicit_followup_not_an_unrelated_subsystem(self):
        for previous, object_class, tag in (
            (1140, 'Planet', 'Corruption_2_Required_Unit_Count'),
            (679, 'StarBase', 'Behavior'),
            (1114, 'TradeRoute', 'Political_Control_Gain'),
            (2393, 'GameConstants', 'BeaconPlaceDelay'),
            (530, 'SpaceUnit', 'Score_Cost_Credits'),
        ):
            with self.subTest(tag=tag):
                row = {'status': 'partial', 'ticket': previous, 'missing_types': ['remaining mode']}
                builder.refresh_audit_ticket(row, object_class, tag)
                self.assertEqual(row['ticket'], 4532)
                self.assertEqual(row['missing_types'], ['remaining mode'])


if __name__ == '__main__':
    unittest.main()
