"""PS-02 contact ownership through the production GPU particle path."""
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live_session_test_support import LiveSessionRunner, ROOT, DUEL_CAMERA
from test_area_damage_capture import installed_xml, point_height_replay, rocket_overlay, set_tag
from effect_mode_fixtures import _chunk, _mini, _text, _emitter, _group, build_glow

sys.path.insert(0, str(ROOT / 'tests/assets/fixtures'))
import scene_fixture


def contact_effect(colour, leave=True):
    emitter = _emitter('contact', blend=1, texture='p_eawr_contact.tga',
        position=_group(0), velocity=_group(0), rgb=colour, size=(12, 12), rate=30, lifetime=0.5)
    # Linked positions follow the emitter; zero velocity makes the stream centre an independent witness.
    body = emitter[8:]
    length = struct.unpack_from('<I', body, 4)[0]
    body = _chunk(2, body[8:8 + length] + _mini(0x08, b'\x01')) + body[8 + length:]
    return _chunk(0x900, _chunk(0, _text('contact')) + _chunk(1, struct.pack('<I', 1))
        + _chunk(0x800, _chunk(0x700, body, True), True) + _chunk(2, bytes([leave])), True)


def contact_model():
    original = scene_fixture.alo_bytes([('MeshGloss.fx', 'p_eawr_contact.tga', 90, 90, 180)] * 2)
    offset = 8 + (struct.unpack_from('<I', original, 4)[0] & 0x7fffffff)
    meshes = []
    for name in ('HULL', 'SHIELD'):
        length = struct.unpack_from('<I', original, offset + 4)[0] & 0x7fffffff
        body = original[offset + 8:offset + 8 + length]
        name_length = struct.unpack_from('<I', body, 4)[0]
        rest = bytearray(body[8 + name_length:])
        # The scene box defaults to noncollidable; this witness needs a real collision footprint.
        struct.pack_into('<I', rest, 8 + 36, 1)
        meshes.append(_chunk(0x400, _chunk(0x401, _text(name)) + rest, True))
        offset += 8 + length
    bones = []
    for name, parent, xyz in (('root', -1, (0, 0, 0)), ('contact', 0, (20, 0, -90))):
        matrix = (1, 0, 0, xyz[0], 0, 1, 0, xyz[1], 0, 0, 1, xyz[2])
        bones.append(_chunk(0x202, _chunk(0x203, _text(name))
            + _chunk(0x205, struct.pack('<iI12f', parent, 1, *matrix)), True))
    skeleton = _chunk(0x200, _chunk(0x201, struct.pack('<I', 2)) + b''.join(bones), True)
    connections = _chunk(0x601, _mini(1, struct.pack('<I', 2)) + _mini(4, struct.pack('<I', 0)))
    for mesh in range(2):
        connections += _chunk(0x602, _mini(2, struct.pack('<I', mesh)) + _mini(3, struct.pack('<I', 1)))
    return skeleton + b''.join(meshes) + _chunk(0x600, connections, True)


def contact_animation():
    frames = 61
    metadata = (_mini(4, _text('contact')) + _mini(5, struct.pack('<I', 1))
        + _mini(6, struct.pack('<3f', 20, 0, -90)) + _mini(7, struct.pack('<3f', 60 / 65535, 0, 0))
        + _mini(8, struct.pack('<3f', 1, 1, 1)) + _mini(9, struct.pack('<3f', 0, 0, 0)))
    translation = b''.join(struct.pack('<3H', round(65535 * index / (frames - 1)), 0, 0) for index in range(frames))
    visibility = bytearray((frames + 7) // 8)
    for index in range(frames):
        if not 45 <= index < 55:
            visibility[index // 8] |= 1 << (index % 8)
    track = _chunk(0x1002, _chunk(0x1003, metadata) + _chunk(0x1004, translation)
        + _chunk(0x1007, bytes(visibility)), True)
    info = _mini(1, struct.pack('<I', frames)) + _mini(2, struct.pack('<f', 30)) + _mini(3, struct.pack('<I', 1))
    return _chunk(0x1000, _chunk(0x1001, info) + track, True)


def contact_overlay(game, root, shield, leave=True):
    rocket_overlay(game, root)
    path = root / 'Data/XML/SpaceUnitsCorvettes.xml'
    ships = ET.parse(path)
    target = next(node for node in ships.getroot() if node.get('Name', '').lower() == 'corellian_corvette')
    for tag, value in {'Space_Model_Name': 'p_eawr_contact.alo', 'Scale_Factor': 1,
        'HardPoints': '', 'Shield_Points': 100000 if shield else 0, 'Shield_Refresh_Rate': 0,
        'Damage_Hit_Particles': 'EAWR_Contact_Attached, EAWR_Contact_Free',
        'Shield_Hit_Particles': 'EAWR_Contact_Attached, EAWR_Contact_Free', 'Loop_Idle_Anim_00': 'Yes'}.items():
        set_tag(target, tag, value)
    ships.write(path, encoding='utf-8', xml_declaration=True)
    path = root / 'Data/XML/Projectiles.xml'
    projectiles = ET.parse(path)
    shot = next(node for node in projectiles.getroot() if node.get('Name') == 'Proj_Ship_Diamond_Boron_Missile')
    for tag, value in {'Max_Speed': 100, 'Projectile_Damage': 1, 'Projectile_Blast_Area_Range': 0}.items():
        set_tag(shot, tag, value)
    projectiles.write(path, encoding='utf-8', xml_declaration=True)
    particles = ET.fromstring(installed_xml(game, 'Particles.xml'))
    for name, attached in (('EAWR_Contact_Attached', True), ('EAWR_Contact_Free', False)):
        particle = ET.SubElement(particles, 'Particle', Name=name)
        for tag, value in {'Space_Model_Name': name + '.alo', 'Behavior': 'PARTICLE',
            'Particle_Lifetime_Frames': 120, 'Particle_Attach_To_Collision': 'Yes' if attached else 'No',
            'Scale_Factor': 1}.items():
            set_tag(particle, tag, value)
    ET.ElementTree(particles).write(root / 'Data/XML/Particles.xml', encoding='utf-8', xml_declaration=True)
    models = root / 'Data/Art/Models'
    textures = root / 'Data/Art/Textures'
    models.mkdir(parents=True)
    textures.mkdir(parents=True)
    (models / 'p_eawr_contact.alo').write_bytes(contact_model())
    (models / 'p_eawr_contact_idle_00.ala').write_bytes(contact_animation())
    (textures / 'p_eawr_contact.tga').write_bytes(build_glow())
    for name, colour in (('EAWR_Contact_Attached', (1, 0.2, 0.1)), ('EAWR_Contact_Free', (0.1, 0.8, 1))):
        (models / (name + '.alo')).write_bytes(contact_effect(colour, leave))


def contact_replay(recorded):
    reduced = bytearray(point_height_replay(recorded))
    players = struct.unpack_from('<I', reduced, 48)[0]
    start = 104 + players * 24
    target_owner = struct.unpack_from('<I', reduced, start + 16)[0]
    shooter_owner = struct.unpack_from('<I', reduced, start + 96)[0]
    commands_start = start + 160
    common = lambda tick, owner, sequence, opcode: struct.pack('<QIQBBH', tick, owner, sequence, opcode, 0, 0)
    units = lambda entity: struct.pack('<IIQ', 1, 0, entity)
    commands = [common(0, shooter_owner, 1, 3) + struct.pack('<Q', 5) + units(11),
        common(10, target_owner, 1, 2) + struct.pack('<3q', 850 << 24, -1300 << 24, 160 << 24) + units(5),
        common(65, target_owner, 2, 5) + struct.pack('<3q', 0, -1800 << 24, 160 << 24) + units(5),
        common(90, target_owner, 3, 4) + struct.pack('<qII', 1000000 << 24, 0xffffffff, 0) + units(5)]
    struct.pack_into('<Q', reduced, 40, 140)
    struct.pack_into('<Q', reduced, 64, len(commands))
    return bytes(reduced[:commands_start]) + b''.join(struct.pack('<I', len(body)) + body for body in commands)


class ParticleContactFixtureTests(unittest.TestCase):
    def test_contact_witness_keeps_two_owners_and_four_commands(self):
        replay = contact_replay((ROOT / 'tests/skirmish/fixtures/m2-start.eawr-replay').read_bytes())
        self.assertEqual(struct.unpack_from('<QQ', replay, 56), (2, 4))
        self.assertEqual(struct.unpack_from('<Q', replay, 40)[0], 140)
        self.assertTrue(contact_model())
        self.assertTrue(contact_animation())


@unittest.skipUnless(os.environ.get('EAWR_GODOT_VIEWER_RUNTIME_TEST') and os.environ.get('EAWR_EAW_GAME_ROOT'),
                     'requires installed game and GPU viewer')
class ParticleContactCapture(LiveSessionRunner, unittest.TestCase):
    def test_attached_hit_follows_moving_rotating_bone_and_free_hit_stays(self):
        for shield in (True, False):
            with self.subTest(shield=shield), tempfile.TemporaryDirectory(prefix='eawr-contact-') as temporary:
                directory = Path(temporary)
                overlay = directory / 'mod'
                contact_overlay(Path(os.environ['EAWR_EAW_GAME_ROOT']), overlay, shield)
                args = ('--eawr-mod-root', str(overlay), '--eawr-audio', 'off', '--eawr-map-particle-frames', '140')
                record = directory / 'identity.eawr-replay'
                code, result = self._run(directory, 'identity', (*args, '--eawr-live-ai', 'off',
                    '--eawr-live-ticks', '1', '--eawr-live-replay-out', str(record)))
                self.assertEqual(code, 0, result.get('failure'))
                replay = directory / 'contact.eawr-replay'
                replay.write_bytes(contact_replay(record.read_bytes()))
                # The rig returns text evidence; retain the derived witness for paired eye-check runs.
                import base64
                (directory / 'contact-replay.txt').write_text(base64.b64encode(replay.read_bytes()).decode('ascii'), encoding='ascii')
                code, result = self._run(directory, 'contact', (*args, '--eawr-live-replay', str(replay),
                    '--eawr-live-reveal', 'on', '--eawr-live-player', '2', '--eawr-live-step', '1',
                    '--eawr-live-ticks', '140', '--eawr-environment', 'map', '--eawr-lighting', 'sh',
                    '--eawr-live-capture-ticks', '15,24,40,60,80,100,130'),
                    session=('--eawr-live-session', 'replay'), camera=DUEL_CAMERA)
                retained = os.environ.get('EAWR_CONTACT_CAPTURE_DIR')
                if retained:
                    import shutil
                    destination = Path(retained) / ('shield' if shield else 'damage')
                    destination.mkdir(parents=True, exist_ok=True)
                    for artifact in directory.glob('contact*'):
                        if artifact.is_file():
                            shutil.copy2(artifact, destination / artifact.name)
                self.assertEqual(code, 0, result.get('failure'))
                self.assertTrue(result['live_session']['headless_hashes_equal'])
                contacts = result['battle_effects']['contacts']
                counts = {key: value for key, value in contacts.items() if key != 'samples'}
                self.assertGreater(contacts['attached'], 0, counts)
                self.assertEqual(contacts['missing'], 0, counts)
                samples = [row for row in contacts['samples'] if row['bounds'] and not row['detached']]
                attached = [row for row in samples if row['attached']]
                free = [row for row in samples if not row['attached']]
                self.assertTrue(attached and free, counts)
                for row in attached + free:
                    self.assertLess(math.dist(row['origin'], row['centre']), 0.05, row)
                grouped = {}
                for row in attached:
                    grouped.setdefault(row['handle'], []).append(row)
                self.assertTrue(any(math.dist(rows[0]['origin'], rows[-1]['origin']) > 10
                                    for rows in grouped.values()), 'attached effects must follow target motion')
                grouped = {}
                for row in free:
                    grouped.setdefault(row['handle'], []).append(row)
                for rows in grouped.values():
                    self.assertEqual(rows[0]['origin'], rows[-1]['origin'])
                if shield:
                    self.assertTrue(any(row['bone'] == 1 for row in attached), 'shield contact must retain its mesh bone')
                    self.assertGreater(contacts['hidden'], 0, counts)
                self.assertGreater(contacts['removed'], 0, counts)


if __name__ == '__main__':
    unittest.main()
