"""WAD-37/39/40: manual hardpoint delay through the production viewer."""
import os
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

from test_area_damage_capture import installed_xml, point_height_replay, rocket_overlay, set_tag
from live_session_test_support import LiveSessionRunner, DUEL_CAMERA


def manual_overlay(game, root, delay):
    rocket_overlay(game, root)
    path = root / 'Data/XML/SpaceUnitsCorvettes.xml'
    ships = ET.parse(path)
    ship = next(node for node in ships.getroot() if node.get('Name', '').lower() == 'tartan_patrol_cruiser')
    set_tag(ship, 'Projectile_Types', '')
    set_tag(ship, 'HardPoints', 'HP_Tartan_Cruiser_00')
    ships.write(path, encoding='utf-8', xml_declaration=True)
    hardpoints = ET.fromstring(installed_xml(game, 'HardPoints.xml'))
    gun = next(node for node in hardpoints if node.get('Name', '').lower() == 'hp_tartan_cruiser_00')
    for tag, value in {
        'Requires_Manual_Target_Assignment': 'Yes', 'Manual_Hardpoint_Firing_Cooldown_Secs': 120,
        'Fire_Projectile_Type': 'Proj_Ship_Diamond_Boron_Missile',
        'Fire_Min_Recharge_Seconds': 0, 'Fire_Max_Recharge_Seconds': 0,
        'Fire_Pulse_Count': 1, 'Fire_Range_Distance': 2000,
        'Fire_Cone_Width': 720, 'Fire_Cone_Height': 720,
        'Projectile_Appearance_Delay_Frames': delay,
        'Allow_Opportunity_Fire_When_Idle': 'False',
    }.items():
        set_tag(gun, tag, value)
    ET.ElementTree(hardpoints).write(root / 'Data/XML/HardPoints.xml', encoding='utf-8', xml_declaration=True)


def manual_replay(recorded, ticks):
    reduced = bytearray(point_height_replay(recorded))
    players = struct.unpack_from('<I', reduced, 48)[0]
    start = 104 + players * 24 + 2 * 80
    owner = struct.unpack_from('<I', reduced, 104 + players * 24 + 80 + 16)[0]
    struct.pack_into('<Q', reduced, 40, ticks)
    body = struct.pack('<QIQBBH', 0, owner, 1, 18, 0, 0)
    body += struct.pack('<QIIIIQ', 5, 0, 0, 1, 0, 11)
    return bytes(reduced[:start]) + struct.pack('<I', len(body)) + body


@unittest.skipUnless(os.environ.get('EAWR_GODOT_VIEWER_RUNTIME_TEST')
                     and os.environ.get('EAWR_EAW_GAME_ROOT'), 'requires installed game and GPU viewer')
class ManualDelayCapture(LiveSessionRunner, unittest.TestCase):
    def test_manual_projectile_hidden_until_authored_delay(self):
        for delay in (15, 30):
            with self.subTest(delay=delay), tempfile.TemporaryDirectory(prefix=f'eawr-manual-delay-{delay}-') as temporary:
                directory = Path(temporary)
                overlay = directory / 'synthetic-mod'
                manual_overlay(Path(os.environ['EAWR_EAW_GAME_ROOT']), overlay, delay)
                args = ('--eawr-mod-root', str(overlay), '--eawr-audio', 'off')
                record = directory / 'identity.eawr-replay'
                code, result = self._run(directory, 'identity', (*args, '--eawr-live-ai', 'off',
                    '--eawr-live-ticks', '1', '--eawr-live-replay-out', str(record)))
                self.assertEqual(code, 0, result.get('failure'))
                for early in (True, False):
                    # The tick-zero command commits after targeting, so firing is tick one.
                    ticks = delay if early else 120
                    replay = directory / f'manual-{ticks}.eawr-replay'
                    replay.write_bytes(manual_replay(record.read_bytes(), ticks))
                    captures = (delay - 1, delay) if early else (delay + 1, delay + 2, delay + 3, delay + 10, delay + 30)
                    name = 'held-lit' if early else 'released-lit'
                    code, result = self._run(directory, name, (*args, '--eawr-live-replay', str(replay),
                        '--eawr-live-reveal', 'on', '--eawr-live-player', '2', '--eawr-live-step', '1',
                        '--eawr-map-timed-frames', '1',
                        '--eawr-live-ticks', str(ticks), '--eawr-environment', 'map', '--eawr-lighting', 'sh',
                        '--eawr-live-capture-ticks', ','.join(map(str, captures))),
                        session=('--eawr-live-session', 'replay'), camera=DUEL_CAMERA)
                    self.assertEqual(code, 0, result.get('failure'))
                    self.assertTrue(result['live_session']['headless_hashes_equal'])
                    effects = result['battle_effects']
                    self.assertIsNone(effects['failure'])
                    pool = effects['projectile_models']['Proj_Ship_Diamond_Boron_Missile']
                    self.assertEqual(pool['refused'], 0)
                    if early:
                        # Snapshot t+1 supplies a frame presented at t. Firing at one
                        # expires at delay+1, so the last fully hidden prefix ends at delay.
                        # The map's default 120 timed frames would overrun this prefix.
                        self.assertEqual(result['live_session']['presented_tick'], ticks)
                        self.assertEqual(result['live_session']['latest_tick'], ticks + 1)
                        self.assertEqual(result['live_session']['completed_ticks'], ticks + 1)
                        self.assertEqual(pool['bindings'], 0, pool)
                        self.assertEqual(effects['projectile_models_drawn'], 0)
                        self.assertGreater(effects['projectiles_hidden'], 0)
                    else:
                        self.assertGreater(pool['bindings'], 0, pool)
                        self.assertGreater(effects['projectile_models_drawn'], 0)
                        self.assertTrue(any('DB_Missile_Explosion' in key and count > 0
                                            for key, count in effects['spawned'].items()))
                    for tick in captures:
                        self.assertTrue((directory / f'{name}_t{tick:04d}.png').is_file())


if __name__ == '__main__':
    unittest.main()
