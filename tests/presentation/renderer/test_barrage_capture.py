"""WAD-38 point proxy and projectile override, through the production viewer."""

import os
from pathlib import Path
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

from test_area_damage_capture import point_height_replay, rocket_overlay, set_tag
from live_session_test_support import LiveSessionRunner, DUEL_CAMERA


def barrage_overlay(game, root):
    rocket_overlay(game, root)
    projectiles_path = root / 'Data/XML/Projectiles.xml'
    projectiles = ET.parse(projectiles_path)
    projectile = next(node for node in projectiles.getroot()
                      if node.get('Name', '').lower() == 'proj_ship_diamond_boron_missile_barrage')
    set_tag(projectile, 'Max_Speed', 30)
    projectiles.write(projectiles_path, encoding='utf-8', xml_declaration=True)
    path = root / 'Data/XML/SpaceUnitsCorvettes.xml'
    ships = ET.parse(path)
    ship = next(node for node in ships.getroot()
                if node.get('Name', '').lower() == 'tartan_patrol_cruiser')
    # An ordinary laser and a modelled override make mistaken look selection visible.
    set_tag(ship, 'Projectile_Types', 'Proj_Ship_Turbolaser_Green')
    behavior = next(node for node in ship if node.tag.lower() == 'spacebehavior')
    set_tag(ship, 'SpaceBehavior', (behavior.text or '') + ', BARRAGE')
    abilities = next(node for node in ship if node.tag.lower() == 'unit_abilities_data')
    abilities.clear()
    ability = ET.SubElement(abilities, 'Unit_Ability')
    for tag, value in {
        'Type': 'BARRAGE', 'Expiration_Seconds': 2, 'Recharge_Seconds': 4,
        'Projectile_Types_Override': 'Proj_Ship_Diamond_Boron_Missile_Barrage',
        'Targeting_Fire_Inaccuracy_Fixed_Radius_Override': 0,
        'Target_Position_Z_Offset': -150,
    }.items():
        set_tag(ability, tag, value)
    modifier = ET.SubElement(ability, 'Mod_Multiplier')
    modifier.text = 'FIRE_RATE_MULTIPLIER, 3'
    ships.write(path, encoding='utf-8', xml_declaration=True)


def barrage_replay(recorded):
    reduced = bytearray(point_height_replay(recorded))
    players = struct.unpack_from('<I', reduced, 48)[0]
    command_start = 104 + players * 24 + 2 * 80
    owner = struct.unpack_from('<I', reduced, 104 + players * 24 + 80 + 16)[0]
    scale = 1 << 24
    common = struct.pack('<QIQBBH', 0, owner, 1, 17, 0, 0)
    # Keep the damageable ship before the immune marker on a flat flight line.
    # This witnesses the inherited ordinary impact look; marker/lifetime effects
    # are the separate WAD-07/28 presentation interface. Scatter/Z rules have C++ contracts.
    target_start = 104 + players * 24
    struct.pack_into('<q', reduced, target_start + 40, 0)
    area = struct.pack('<IIqqq', 6, 0, 800 * scale, -1500 * scale, 150 * scale)
    selected = struct.pack('<IIQ', 1, 0, 11)
    body = common + area + selected
    return bytes(reduced[:command_start]) + struct.pack('<I', len(body)) + body


def ion_impact_replay(recorded):
    header = bytearray(recorded[:104])
    version, header_size = struct.unpack_from('<HH', header, 8)
    assert version == 3 and header_size == 104
    players, squadrons = struct.unpack_from('<II', header, 48)
    unit_count = struct.unpack_from('<Q', header, 56)[0]
    unit_start = header_size + players * 24
    squadron_start = unit_start + unit_count * 80
    members = None
    for _ in range(squadrons):
        container, count = struct.unpack_from('<QI', recorded, squadron_start)
        if container == 4:
            members = struct.unpack_from('<' + 'Q' * count, recorded, squadron_start + 16)
        squadron_start += 16 + count * 8
    assert members
    selected = {4, 5, 11, *members}
    retained = []
    scale = 1 << 24
    owner = None
    for index in range(unit_count):
        start = unit_start + index * 80
        entity = struct.unpack_from('<Q', recorded, start)[0]
        if entity not in selected:
            continue
        unit = bytearray(recorded[start:start + 80])
        # Keep the recorded ion members and one allied spotter beside the victim.
        x, y = (400, -1500) if entity == 11 else (-400, -1500)
        if entity == 5:
            y = -1000
        if entity == 4:
            owner = struct.unpack_from('<I', unit, 16)[0]
        struct.pack_into('<qqqqqqq', unit, 24, x * scale, y * scale, 0, 0, 0, 0, scale)
        retained.append(unit)
    assert owner is not None
    struct.pack_into('<Q', header, 40, 120)
    struct.pack_into('<I', header, 52, 1)
    struct.pack_into('<QQ', header, 56, len(retained), 1)
    squadron = struct.pack('<QII', 4, len(members), 0) + struct.pack('<' + 'Q' * len(members), *members)
    common = struct.pack('<QIQBBH', 0, owner, 1, 8, 0, 0)
    body = common + struct.pack('<BBHIQIIQ', 5, 1, 1, 0xffffffff, 11, 1, 0, 4)
    return (bytes(header) + recorded[header_size:unit_start] + b''.join(retained) + squadron
            + struct.pack('<I', len(body)) + body)


@unittest.skipUnless(os.environ.get('EAWR_GODOT_VIEWER_RUNTIME_TEST')
                     and os.environ.get('EAWR_EAW_GAME_ROOT'), 'requires installed game and GPU viewer')
class BarrageCapture(LiveSessionRunner, unittest.TestCase):
    def test_ion_override_keeps_its_own_impact_look(self):
        with tempfile.TemporaryDirectory(prefix='eawr-ion-impact-') as temporary:
            directory = Path(temporary)
            overlay = directory / 'synthetic-mod'
            barrage_overlay(Path(os.environ['EAWR_EAW_GAME_ROOT']), overlay)
            path = overlay / 'Data/XML/Projectiles.xml'
            projectiles = ET.parse(path)
            ion = next(node for node in projectiles.getroot()
                       if node.get('Name', '') == 'Proj_Ion_Cannon_Medium_Laser_Blue')
            # A unique supported particle separates the ion look from each ordinary craft weapon.
            for tag in ('Projectile_Object_Detonation_Particle', 'Projectile_Absorbed_By_Shields_Particle',
                        'Projectile_Object_Armor_Reduced_Detonation_Particle'):
                set_tag(ion, tag, 'DB_Missile_Explosion')
            projectiles.write(path, encoding='utf-8', xml_declaration=True)
            record = directory / 'identity.eawr-replay'
            args = ('--eawr-mod-root', str(overlay), '--eawr-audio', 'off')
            code, result = self._run(directory, 'ion-identity', (*args, '--eawr-live-ai', 'off',
                '--eawr-live-ticks', '1', '--eawr-live-replay-out', str(record)))
            self.assertEqual(code, 0, result.get('failure'))
            replay = directory / 'ion-impact.eawr-replay'
            replay.write_bytes(ion_impact_replay(record.read_bytes()))
            code, result = self._run(directory, 'ion-impact-lit', (*args,
                '--eawr-live-replay', str(replay), '--eawr-live-reveal', 'on', '--eawr-live-player', '1',
                '--eawr-live-step', '1', '--eawr-live-ticks', '120', '--eawr-environment', 'map',
                '--eawr-lighting', 'sh'), session=('--eawr-live-session', 'replay'), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get('failure'))
            self.assertTrue(result['live_session']['headless_hashes_equal'])
            effects = result['battle_effects']
            self.assertIsNone(effects['failure'])
            self.assertEqual(set(effects['ability_shots_fired']), {'Proj_Ion_Cannon_Medium_Laser_Blue'})
            self.assertTrue(any('DB_Missile_Explosion' in key and count > 0
                                for key, count in effects['spawned'].items()), effects['spawned'])

    def test_point_proxy_uses_override_rocket_look(self):
        with tempfile.TemporaryDirectory(prefix='eawr-barrage-') as temporary:
            directory = Path(temporary)
            overlay = directory / 'synthetic-mod'
            barrage_overlay(Path(os.environ['EAWR_EAW_GAME_ROOT']), overlay)
            record = directory / 'identity.eawr-replay'
            args = ('--eawr-mod-root', str(overlay), '--eawr-audio', 'off')
            code, result = self._run(directory, 'identity', (*args, '--eawr-live-ai', 'off',
                '--eawr-live-ticks', '1', '--eawr-live-replay-out', str(record)))
            self.assertEqual(code, 0, result.get('failure'))
            replay = directory / 'point-barrage.eawr-replay'
            replay.write_bytes(barrage_replay(record.read_bytes()))
            ticks = (4, 12, 20, 28, 40, 48, 56, 60, 72, 96)
            code, result = self._run(directory, 'barrage-lit', (*args,
                '--eawr-live-replay', str(replay), '--eawr-live-reveal', 'on', '--eawr-live-player', '2',
                '--eawr-live-step', '1', '--eawr-live-ticks', '120', '--eawr-environment', 'map',
                '--eawr-lighting', 'sh', '--eawr-live-capture-ticks', ','.join(map(str, ticks))),
                session=('--eawr-live-session', 'replay'), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get('failure'))
            self.assertTrue(result['live_session']['headless_hashes_equal'])
            effects = result['battle_effects']
            self.assertIsNone(effects['failure'])
            name = 'Proj_Ship_Diamond_Boron_Missile_Barrage'
            self.assertGreater(effects['ability_shots_fired'].get(name, 0), 0)
            self.assertGreater(effects['ability_shot_frames_drawn'].get(name, 0), 0)
            pool = effects['projectile_models'][name]
            self.assertGreater(pool['bindings'], 0)
            self.assertEqual(pool['refused'], 0)
            self.assertTrue(any('DB_Missile_Explosion' in key and count > 0
                                for key, count in effects['spawned'].items()))
            for tick in ticks:
                self.assertTrue((directory / f'barrage-lit_t{tick:04d}.png').is_file())


if __name__ == '__main__':
    unittest.main()
