"""WAD-07 terminal particles through ordinary and point-barrage projectile looks."""

import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))

from test_area_damage_capture import point_height_replay, rocket_overlay, set_tag
from test_barrage_capture import barrage_overlay, barrage_replay
from live_session_test_support import LiveSessionRunner, DUEL_CAMERA


def expiry_overlay(game, root, route, barrage=False):
    (barrage_overlay if barrage else rocket_overlay)(game, root)
    path = root / 'Data/XML/SpaceUnitsCorvettes.xml'
    ships = ET.parse(path)
    target = next(node for node in ships.getroot()
                  if node.get('Name', '').lower() == 'corellian_corvette')
    impact = route in ('shield', 'hull', 'reduced')
    set_tag(target, 'Collidable_By_Projectile_Living', 'Yes' if impact else 'No')
    if impact:
        # Difficulty scaling must keep the synthetic durability within its bound.
        set_tag(target, 'Tactical_Health', 100000)
        set_tag(target, 'Shield_Points', 100000 if route == 'shield' else 0)
        set_tag(target, 'Shield_Refresh_Rate', 0)
        if route == 'reduced':
            set_tag(target, 'Armor_Type', 'Armor_Nebulon_B')
    ships.write(path, encoding='utf-8', xml_declaration=True)
    path = root / 'Data/XML/Projectiles.xml'
    projectiles = ET.parse(path)
    name = 'Proj_Ship_Diamond_Boron_Missile' + ('_Barrage' if barrage else '')
    projectile = next(node for node in projectiles.getroot() if node.get('Name') == name)
    distance = {'travel': 120, 'rocket_radius': 200, 'lifetime': 0}.get(route, 5000)
    set_tag(projectile, 'Projectile_Max_Flight_Distance', distance)
    if route in ('travel', 'default_radius', 'lifetime'):
        set_tag(projectile, 'Projectile_Category', 'DEFAULT')
    if route == 'travel':
        set_tag(projectile, 'Explode_When_Reached_Target_Radius', 'No')
    if route == 'lifetime':
        set_tag(projectile, 'Explode_When_Reached_Target_Radius', 'No')
        set_tag(projectile, 'Projectile_Max_Lifetime', 0.1)
    projectiles.write(path, encoding='utf-8', xml_declaration=True)


def expiry_replay(recorded, barrage=False, fog=False):
    replay = bytearray((barrage_replay if barrage else point_height_replay)(recorded))
    players = struct.unpack_from('<I', replay, 48)[0]
    # A flat line separates lifetime from the launch height-distance allowance.
    struct.pack_into('<q', replay, 104 + players * 24 + 40, 0)
    if fog:
        start = 104 + players * 24
        # A third observer-owned corvette reveals the enemy shooter, but not the
        # short flight's terminal pose. The target reveals only its own endpoint.
        supporter = bytearray(replay[start:start + 80])
        struct.pack_into('<Q', supporter, 0, 99)
        struct.pack_into('<qqq', supporter, 24, -600 * (1 << 24), -1500 * (1 << 24), 0)
        replay[start + 160:start + 160] = supporter
        struct.pack_into('<Q', replay, 56, 3)
    return bytes(replay)


@unittest.skipUnless(os.environ.get('EAWR_GODOT_VIEWER_RUNTIME_TEST')
                     and os.environ.get('EAWR_EAW_GAME_ROOT'), 'requires installed game and GPU viewer')
class ProjectileExpiryCapture(LiveSessionRunner, unittest.TestCase):
    def test_terminal_routes_keep_the_diamond_boron_lifetime_look(self):
        # The explicit-request terminal path has the same event consumer; its
        # trigger and final pose are covered by the engine-free WAD-07 contract.
        for route, barrage in (('travel', False), ('rocket_radius', False),
                               ('path', False), ('default_radius', False),
                               ('path', True), ('lifetime', True),
                               ('shield', False), ('hull', False), ('reduced', False)):
            with self.subTest(route=route, barrage=barrage):
                self._terminal_route(route, barrage)

    def test_uncreated_lifetime_effects_request_no_sound(self):
        for admission in ('absent', 'torpedo', 'unresolved', 'frustum', 'fog', 'fog_nohide'):
            with self.subTest(admission=admission):
                self._terminal_route('travel', False, admission)

    def test_created_lifetime_effect_allocates_authored_audio(self):
        self._terminal_route('travel', False, 'audible')

    def _terminal_route(self, route, barrage, admission='created'):
        fogged = admission in ('fog', 'fog_nohide')
        with tempfile.TemporaryDirectory(prefix='eawr-projectile-expiry-') as temporary:
            directory = Path(temporary)
            overlay = directory / 'synthetic-mod'
            expiry_overlay(Path(os.environ['EAWR_EAW_GAME_ROOT']), overlay, route, barrage)
            if admission in ('absent', 'unresolved'):
                path = overlay / 'Data/XML/Projectiles.xml'
                projectiles = ET.parse(path)
                projectile = next(node for node in projectiles.getroot()
                                  if node.get('Name') == 'Proj_Ship_Diamond_Boron_Missile')
                set_tag(projectile, 'Projectile_Lifetime_Detonation_Particle',
                        '' if admission == 'absent' else 'EAWR_Unresolved_Lifetime_Effect')
                projectiles.write(path, encoding='utf-8', xml_declaration=True)
            if fogged or admission == 'torpedo':
                path = overlay / 'Data/XML/SpaceUnitsCorvettes.xml'
                ships = ET.parse(path)
                for ship in ships.getroot():
                    name = ship.get('Name', '').lower()
                    if fogged and name in ('tartan_patrol_cruiser', 'corellian_corvette'):
                        set_tag(ship, 'Space_FOW_Reveal_Range', 220)
                    if admission == 'torpedo' and name == 'tartan_patrol_cruiser':
                        set_tag(ship, 'Projectile_Types', 'Proj_Ship_Proton_Torpedo')
                ships.write(path, encoding='utf-8', xml_declaration=True)
                if fogged:
                    path = overlay / 'Data/XML/Projectiles.xml'
                    projectiles = ET.parse(path)
                    projectile = next(node for node in projectiles.getroot()
                                      if node.get('Name') == 'Proj_Ship_Diamond_Boron_Missile')
                    set_tag(projectile, 'Projectile_Max_Flight_Distance', 400)
                    if admission == 'fog_nohide':
                        set_tag(projectile, 'Behavior', 'PROJECTILE')
                    projectiles.write(path, encoding='utf-8', xml_declaration=True)
                if admission == 'torpedo':
                    path = overlay / 'Data/XML/Projectiles.xml'
                    projectiles = ET.parse(path)
                    projectile = next(node for node in projectiles.getroot()
                                      if node.get('Name') == 'Proj_Ship_Proton_Torpedo')
                    set_tag(projectile, 'Projectile_Max_Lifetime', 0.1)
                    projectiles.write(path, encoding='utf-8', xml_declaration=True)
            record = directory / 'identity.eawr-replay'
            args = ('--eawr-mod-root', str(overlay), '--eawr-audio',
                    'on' if admission == 'audible' else 'off')
            if admission == 'audible':
                args += ('--eawr-live-audio-pace', 'on')
            code, result = self._run(directory, 'identity', (*args, '--eawr-live-ai', 'off',
                '--eawr-live-ticks', '1', '--eawr-live-replay-out', str(record)))
            self.assertEqual(code, 0, result.get('failure'))
            replay = directory / 'expiry.eawr-replay'
            replay.write_bytes(expiry_replay(record.read_bytes(), barrage, fogged))
            camera = DUEL_CAMERA
            if barrage or admission == 'frustum':
                shutil.copyfile(DUEL_CAMERA.parent / 'space-live-camera-bindings.json',
                                directory / 'space-live-camera-bindings.json')
            if barrage:
                # The barrage marker is at x=800; keep it and the x=-400 source
                # inside the view now that terminal decoration culls are applied.
                camera = directory / 'barrage-camera.xml'
                policy = ET.parse(DUEL_CAMERA)
                policy.getroot().find('initial').attrib.update(target_x='200', zoom='0.55')
                policy.write(camera, encoding='utf-8', xml_declaration=True)
            if admission == 'frustum':
                camera = directory / 'offscreen-camera.xml'
                policy = ET.parse(DUEL_CAMERA)
                policy.getroot().find('initial').set('target_y', '4500')
                policy.write(camera, encoding='utf-8', xml_declaration=True)
            code, result = self._run(directory, 'expiry-lit', (*args,
                '--eawr-live-replay', str(replay), '--eawr-live-reveal', 'off' if fogged else 'on',
                '--eawr-live-player', '1' if fogged else '2',
                '--eawr-live-step', '1', '--eawr-live-ticks', '120', '--eawr-environment', 'map',
                '--eawr-lighting', 'sh', '--eawr-live-capture-ticks', '8,16,32,48,64,96'),
                session=('--eawr-live-session', 'replay'), camera=camera)
            self.assertEqual(code, 0, result.get('failure'))
            self.assertTrue(result['live_session']['headless_hashes_equal'])
            effects = result['battle_effects']
            self.assertGreater(effects['projectile_models_drawn'] + effects['projectiles_drawn']
                               + effects['projectiles_hidden'], 0, 'the witness must launch projectiles')
            self.assertIsNone(effects['failure'])
            lifetime = {key: count for key, count in effects['spawned'].items()
                        if key.startswith('lifetime_detonation:')}
            sound = result['battle_audio']['requested'].get(
                'lifetime_detonation:SFX_Concussion_Missile_Detonation', 0)
            self.assertEqual(sound, sum(lifetime.values()), result['battle_audio']['requested'])
            self.assertEqual(sound, sum(count for key, count in result['battle_audio']['requested'].items()
                                        if key.startswith('lifetime_detonation:')))
            if admission == 'audible':
                self.assertGreater(result['battle_audio']['allocations']['lifetime_detonation']['audible'], 0)
                starts = [row for row in result['battle_audio']['ability_starts']
                          if row['reason'] == 'lifetime_detonation']
                self.assertTrue(starts)
                for start in starts:
                    self.assertEqual(start['event'], 'SFX_Concussion_Missile_Detonation')
                    self.assertGreater(start['position'][0], -400)
                    self.assertAlmostEqual(start['position'][1], -1500)
                    self.assertAlmostEqual(start['position'][2], 0)
            if admission not in ('created', 'audible'):
                self.assertEqual(sum(lifetime.values()), 0, lifetime)
                if admission == 'unresolved':
                    self.assertGreater(effects['spawn_failed'].get(
                        'lifetime_detonation:EAWR_Unresolved_Lifetime_Effect', 0), 0)
                return
            if route in ('shield', 'hull', 'reduced'):
                reason = 'shield:' if route == 'shield' else 'detonation:'
                self.assertTrue(any(key.startswith(reason) and 'DB_Missile_Explosion' in key
                                    and count > 0 for key, count in effects['spawned'].items()),
                                effects['spawned'])
                self.assertFalse(any(count > 0 for count in lifetime.values()), lifetime)
                return
            self.assertTrue(any('DB_Missile_Explosion' in key and count > 0
                                for key, count in lifetime.items()), effects['spawned'])
            self.assertFalse(any(key.startswith(('detonation:', 'shield:', 'armor_reduced:'))
                                 and 'DB_Missile_Explosion' in key and count > 0
                                 for key, count in effects['spawned'].items()),
                             effects['spawned'])


def fog_overlay(game, root, hide=True):
    expiry_overlay(game, root, 'travel')
    path = root / 'Data/XML/SpaceUnitsCorvettes.xml'
    ships = ET.parse(path)
    for ship in ships.getroot():
        name = ship.get('Name', '').lower()
        if name == 'corellian_corvette':
            set_tag(ship, 'Space_FOW_Reveal_Range', 100)
            set_tag(ship, 'HardPoints', '')
            set_tag(ship, 'Projectile_Types', '')
        elif name == 'tartan_patrol_cruiser':
            set_tag(ship, 'Space_FOW_Reveal_Range', 3000)
            set_tag(ship, 'Targeting_Max_Attack_Distance', 3000)
    ships.write(path, encoding='utf-8', xml_declaration=True)
    path = root / 'Data/XML/Projectiles.xml'
    projectiles = ET.parse(path)
    shot = next(node for node in projectiles.getroot()
                if node.get('Name') == 'Proj_Ship_Diamond_Boron_Missile')
    set_tag(shot, 'Behavior', 'PROJECTILE, HIDE_WHEN_FOGGED' if hide else 'PROJECTILE')
    set_tag(shot, 'Last_State_Visible_Under_FOW', 'Yes')
    set_tag(shot, 'Projectile_Max_Flight_Distance', 1200)
    projectiles.write(path, encoding='utf-8', xml_declaration=True)


def fog_replay(recorded):
    replay = bytearray(expiry_replay(recorded))
    start = 104 + struct.unpack_from('<I', replay, 48)[0] * 24
    scale = 1 << 24
    struct.pack_into('<qqq', replay, start + 24, 1000 * scale, -1500 * scale, 0)
    struct.pack_into('<qqq', replay, start + 80 + 24, -1000 * scale, -1500 * scale, 0)
    return bytes(replay)


@unittest.skipUnless(os.environ.get('EAWR_GODOT_VIEWER_RUNTIME_TEST')
                     and os.environ.get('EAWR_EAW_GAME_ROOT'), 'requires installed game and GPU viewer')
class ProjectileFogCapture(LiveSessionRunner, unittest.TestCase):
    def test_hidden_flight_and_expiry_use_the_observer_grid(self):
        self._fog_case(True)

    def test_decoration_fog_admission_is_independent_of_projectile_hide(self):
        self._fog_case(False)

    def _fog_case(self, hide):
        with tempfile.TemporaryDirectory(prefix='eawr-projectile-fog-') as temporary:
            directory = Path(temporary)
            overlay = directory / 'synthetic-mod'
            fog_overlay(Path(os.environ['EAWR_EAW_GAME_ROOT']), overlay, hide)
            record = directory / 'identity.eawr-replay'
            args = ('--eawr-mod-root', str(overlay), '--eawr-audio', 'off')
            code, result = self._run(directory, 'identity', (*args, '--eawr-live-ai', 'off',
                '--eawr-live-ticks', '1', '--eawr-live-replay-out', str(record)))
            self.assertEqual(code, 0, result.get('failure'))
            replay = directory / 'fog.eawr-replay'
            replay.write_bytes(fog_replay(record.read_bytes()))
            reports = {}
            for reveal in ('off', 'on'):
                code, result = self._run(directory, 'fog-' + reveal + '-lit', (*args,
                    '--eawr-live-replay', str(replay), '--eawr-live-reveal', reveal,
                    '--eawr-live-player', '1', '--eawr-live-step', '1', '--eawr-live-ticks', '120',
                    '--eawr-environment', 'map', '--eawr-lighting', 'sh', '--eawr-shadows', 'on',
                    '--eawr-live-capture-ticks', '8,32,48,64'),
                    session=('--eawr-live-session', 'replay'), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get('failure'))
                self.assertTrue(result['live_session']['headless_hashes_equal'], reveal)
                self.assertIsNone(result['battle_effects']['failure'])
                if reveal == 'off':
                    # V-19 firing flashes reveal the enemy source cell; the own
                    # target stays visible. Neither clears intermediate cells.
                    self.assertEqual(result['live_session']['visible_units'], 2,
                                     'both endpoints must remain visible in the fog-on witness')
                reports[reveal] = result['battle_effects']
            hidden, shown = reports['off'], reports['on']
            if hide:
                self.assertGreater(hidden['projectiles_hidden'], 0)
                self.assertLess(hidden['projectile_models_drawn'], shown['projectile_models_drawn'])
            else:
                self.assertEqual(hidden['projectiles_hidden'], 0)
                self.assertEqual(hidden['projectile_models_drawn'], shown['projectile_models_drawn'],
                                 'without HIDE_WHEN_FOGGED, flight draws but decoration fog culls still apply')
            self.assertGreater(hidden['projectile_models_drawn'], 0,
                               'the source firing flash admits visible launch flight before the fogged gap')
            self.assertGreater(shown['projectile_models_drawn'], 0)
            lifetime = lambda effects: sum(count for name, count in effects['spawned'].items()
                if name.startswith('lifetime_detonation:') and 'DB_Missile_Explosion' in name)
            self.assertEqual(lifetime(hidden), 0, hidden['spawned'])
            self.assertGreater(lifetime(shown), 0, shown['spawned'])



CONTRACT_ROOT = Path(os.environ.get('EAWR_EXPIRY_CONTRACT_ROOT', Path(__file__).resolve().parents[3]))


def sound_source(name):
    return (CONTRACT_ROOT / 'apps/viewer/src' / name).read_text(encoding='utf-8')


class ProjectileExpirySoundContract(unittest.TestCase):
    def setUp(self):
        self.impacts = sound_source('battle_effects_impacts.cpp')
        self.spawn = self.impacts.split('bool BattleEffects::spawn(', 1)[1].split(
            'bool BattleEffects::step_effect(', 1)[0]
        self.terminal = self.impacts.split('CombatEventKind::projectile_expired)', 1)[1].split(
            'if (event.kind != tactical::CombatEventKind::projectile_hit)', 1)[0]

    def test_absent_and_unresolved_effects_cannot_publish_audio(self):
        self.assertLess(self.spawn.index('*created = false'), self.spawn.index('if (particle.empty())'))
        self.assertLess(self.spawn.index('type == nullptr || !type->system'), self.spawn.index('*created = true'))
        self.assertIn('if (created) terminal_sounds_.push_back', self.terminal)

    def test_backend_creation_failure_cannot_publish_audio(self):
        for failure in ('if (!handle)', 'if (!registry_->set_frame', 'effects_.size() >= max_live_effects',
                        '++expired_[key]'):
            with self.subTest(failure=failure):
                self.assertLess(self.spawn.index(failure), self.spawn.index('*created = true'))
        self.assertLess(self.spawn.index('if (!step_effect(effect, gone)) return false'),
                        self.spawn.index('*created = true'))

    def test_fog_and_frustum_admission_precedes_creation_and_audio(self):
        self.assertLess(self.terminal.index('projectile_terminal_admitted'), self.terminal.index('if (!spawn('))
        gate = self.impacts.split('bool terminal_in_frustum(', 1)[1].split('particles::Basis3 yaw_basis', 1)[0]
        self.assertIn('projectile_observer_', sound_source('battle_effects_projectiles.cpp'))
        self.assertIn('camera.near_plane', gate)
        self.assertIn('camera.far_plane', gate)
        self.assertIn('height * aspect', gate)
        self.assertIn('terminal->decoration', self.terminal)

    def test_success_preserves_projectile_identity_and_terminal_pose(self):
        self.assertIn('look_of(projectile)', self.terminal)
        self.assertIn('projectile.id = event.target', self.terminal)
        self.assertIn('const V position = vec(event.aim)', self.terminal)
        self.assertIn('{look->projectile, {position.x, position.y, position.z},', self.terminal)
        self.assertIn('terminal_detonations_.find(skirmish::type_id(terminal.projectile))', sound_source('battle_audio_events.cpp'))
        self.assertIn('"Projectile_SFXEvent_Detonate"', sound_source('battle_audio_prepare.cpp').split(
            'terminal_detonations_.emplace', 1)[1])

    def test_explicit_detonation_uses_the_same_success_receipt(self):
        # Both terminal reasons arrive as projectile_expired, including WPJ-37.
        self.assertNotIn('event.outcome', self.terminal)
        self.assertIn('&created', self.terminal)
        self.assertIn('"lifetime_detonation"', sound_source('battle_audio_events.cpp'))

    def test_hit_route_cannot_publish_an_expiry_receipt(self):
        hit = self.impacts.split('if (event.kind != tactical::CombatEventKind::projectile_hit)', 1)[1]
        self.assertNotIn('terminal_sounds_.push_back', hit)
        self.assertIn('terminal_sounds_.clear()', self.impacts)
        self.assertIn('std::exchange(terminal_sounds_, {})', sound_source('battle_effects.hpp'))

    def test_effects_run_before_the_single_audio_consumer(self):
        caller = sound_source('map_mode_ready_population.cpp')
        self.assertLess(caller.index('const bool shown = effects->frame'), caller.index('sound->frame('))
        self.assertEqual(caller.count('effects->take_terminal_sounds()'), 1)




if __name__ == '__main__':
    unittest.main()
