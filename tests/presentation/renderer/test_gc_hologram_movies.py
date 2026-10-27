"""Authored campaign briefing movies through the shared HUD player, with safe failures."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import wave
import xml.etree.ElementTree as ET

from tests.presentation.renderer.campaign_space_cases import qualified_campaign_arguments
from tests.presentation.renderer.test_viewer_planet_hud_runtime import ROOT, RUNTIME, GAME_ROOT
from tools.inventory.corpus import Corpus


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class CampaignHologramMovies(unittest.TestCase):
    def run_movie(self, variant):
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-hologram-" + variant + "-"))
        xml = directory / "overlay/Data/XML"
        xml.mkdir(parents=True)
        corpus = Corpus(GAME_ROOT)
        factions = ET.fromstring(corpus.read_effective("foc", "data/xml/factions.xml").data)
        faction = next(row for row in factions if row.attrib.get("Name") == "Rebel")
        for row in list(faction):
            if row.tag.startswith("SpeechEvent_Tactical_Intro_Space_") or row.tag == "Tactical_Intro_Command_Bar_Movie_Name":
                faction.remove(row)
        ET.SubElement(faction, "SpeechEvent_Tactical_Intro_Space_Attacker").text = "EAWR_TOAST_SPEECH"
        ET.SubElement(faction, "Tactical_Intro_Command_Bar_Movie_Name").text = "EAWR_TOAST_MOVIE"
        ET.ElementTree(factions).write(xml / "Factions.xml", encoding="utf-8")
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>")
        speech = xml.parent / "Audio/Speech/English"
        speech.mkdir(parents=True)
        with wave.open(str(speech / "toast.wav"), "wb") as stream:
            stream.setnchannels(1)
            stream.setsampwidth(2)
            stream.setframerate(8000)
            stream.writeframes(b"\x80\x00" * (8000 * 25))
        (xml / "SpeechEvents.xml").write_text(
            '<SpeechEvents><SpeechEvent Name="EAWR_TOAST_SPEECH"><Files>toast.wav</Files>'
            '<Volume_Percent>10</Volume_Percent></SpeechEvent></SpeechEvents>')
        # Optional private retail conversion for the eye check; default uses only
        # the repository's authored four-colour/alpha fixture.
        private_movie = os.environ.get("EAWR_JOB_TOAST_OGV")
        if private_movie:
            movies = ET.fromstring(corpus.read_effective("foc", "data/xml/movies.xml").data)
            portrait = next(row for row in movies if row.attrib.get("Name", "").lower() == "luke_loop")
            file_name = portrait.findtext("Movie_File").strip().replace("\\", "/").lower()
            source = corpus.read_effective("foc", file_name).data
            portrait.set("Name", "EAWR_TOAST_MOVIE")
            root = ET.Element("Movies")
            root.append(portrait)
            ET.ElementTree(root).write(xml / "Movies.xml", encoding="utf-8")
        else:
            source = b"BIKi synthetic campaign portrait"
            target = xml.parent / "Art/Movies/Binked/portrait.bik"
            target.parent.mkdir(parents=True)
            target.write_bytes(source)
            (xml / "Movies.xml").write_text('<Movies><Movie Name="EAWR_TOAST_MOVIE">'
                '<Movie_File>Data/Art/Movies/Binked/portrait.bik</Movie_File><Alpha>Yes</Alpha></Movie></Movies>')
        cache = directory / "cache"
        cache.mkdir()
        key = hashlib.sha256(source).hexdigest() + "-hud-v1-alpha.ogv"
        if variant == "valid":
            if private_movie:
                shutil.copyfile(private_movie, cache / key)
            else:
                (cache / key).write_bytes(base64.b64decode((ROOT / "tests/ui/fixtures/hud_movie_alpha.ogv.base64").read_bytes()))
        elif variant == "undecodable":
            (cache / key).write_bytes(b"OggS invalid Theora stream")
        elif variant == "missing":
            (xml / "Movies.xml").write_text('<Movies/>')
        driver = directory / "drive.gd"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var tactical_age = 0.0
var movie_age = 0.0
var paused_at = -1.0
var pause_clicked = false
var resumed = false
var pause_drift = 0.0
var seen = false
var frames = {}
var movie_rect = []
var previous_usec = Time.get_ticks_usec()
var wall_frames = []
var movie_frame = -1
var sample_frame = 0
var expect_movie = %s
var warmup_seen = false
var warmup_draws = 0
var warmup_alpha = 0.0
var warmup_trace = []
var had_warmup = false
var warmup_end_frame = -1
var first_battle_frame = -1
var first_battle_ms = 0.0
func _initialize():
    change_scene_to_file("res://main.tscn")
func click_pause():
    var button = root.find_child("b_play_pause_t", true, false)
    if button == null: return
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = button.get_global_rect().get_center()
        event.global_position = event.position
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
        root.get_viewport().push_input(event, true)
func _process(delta):
    var now = Time.get_ticks_usec()
    var wall_frame_ms = (now - previous_usec) / 1000.0
    previous_usec = now
    sample_frame += 1
    elapsed += delta
    var tactical = root.find_child("EawrTacticalHud", true, false) != null
    if tactical: tactical_age += delta
    var screen = root.find_child("EawrLoadingScreen", true, false)
    var loading = screen != null and screen.is_visible_in_tree() and screen.get_parent().is_visible()
    if tactical and not loading and first_battle_frame < 0:
        first_battle_frame = sample_frame
        first_battle_ms = wall_frame_ms
    var warming = root.find_child("EAWR_HUD_MOVIE_WARMUP", true, false)
    if warming != null: had_warmup = true
    elif had_warmup and warmup_end_frame < 0: warmup_end_frame = sample_frame
    if warming != null or (warmup_end_frame >= 0 and sample_frame <= warmup_end_frame + 3):
        warmup_trace.append({"frame":sample_frame,"wall_frame_ms":wall_frame_ms,"loading":loading,"warming":warming != null})
    if warming != null and warming.is_visible_in_tree() and warming.modulate.a > 0:
        var texture = warming.get_video_texture()
        if texture != null and texture.get_width() > 0:
            warmup_seen = true
            warmup_alpha = warming.modulate.a
    var player = root.find_child("EAWR_TOAST_MOVIE", true, false)
    if player != null: warmup_draws = int(player.get_meta("eawr_movie_warmup_frames", 0))
    if player != null and movie_frame < 0: movie_frame = sample_frame
    if movie_frame < 0 or sample_frame <= movie_frame + 40:
        wall_frames.append({"frame":sample_frame,"wall_frame_ms":wall_frame_ms,"movie":player != null})
        if movie_frame < 0 and wall_frames.size() > 40: wall_frames.pop_front()
    if expect_movie and player != null and player.get_video_texture() != null:
        seen = true
        movie_age += delta
        # Keep startup frame timing free of capture/readback work.
        if movie_age > 0.2 and sample_frame %% 4 == 0:
            var image = player.get_video_texture().get_image()
            if image != null: frames[hash(image.get_data())] = true
        var rect = player.get_global_rect()
        movie_rect = [rect.position.x, rect.position.y, rect.size.x, rect.size.y]
        if movie_age > 0.3 and not pause_clicked:
            root.get_viewport().get_texture().get_image().save_png(%s + "/hologram.png")
            click_pause()
            pause_clicked = true
        if player.is_paused():
            if paused_at < 0: paused_at = player.get_stream_position()
            else: pause_drift = max(pause_drift, abs(player.get_stream_position() - paused_at))
        if movie_age > 1.0 and not resumed:
            click_pause()
            resumed = true
    if movie_age > 1.6 or (not expect_movie and tactical_age > 18.0) or elapsed > 180:
        var speech_playing = false
        for voice in root.find_children("*", "AudioStreamPlayer", true, false):
            var stream = voice.get_stream()
            if voice.get_bus() == "EAWR_Voice" and stream != null and abs(stream.get_length() - 25.0) < 0.1:
                speech_playing = speech_playing or voice.is_playing()
        var file = FileAccess.open(%s + "/driver.json", FileAccess.WRITE)
        file.store_string(JSON.stringify({"seen":seen,"frames":frames.size(),"paused":paused_at >= 0,"pause_drift":pause_drift,"resumed":resumed,"rect":movie_rect,"movie_frame":movie_frame,"wall_frames":wall_frames,"speech_playing":speech_playing,"movie_present":player != null,"warmup_seen":warmup_seen,"warmup_draws":warmup_draws,"warmup_alpha":warmup_alpha,"warmup_end_frame":warmup_end_frame,"first_battle_frame":first_battle_frame,"first_battle_ms":first_battle_ms,"warmup_trace":warmup_trace}))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % ("true" if variant == "valid" else "false", json.dumps(directory.as_posix()),
       json.dumps(directory.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate",
            "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            *qualified_campaign_arguments("win"), "--eawr-game-root", GAME_ROOT,
            "--eawr-mod-root", str(directory / "overlay"), "--eawr-movie-cache", str(cache),
            "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
            "--eawr-report", str(directory / "report.json")], cwd=ROOT,
            capture_output=True, text=True, timeout=600, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((directory / "report.json").read_text(encoding="utf-8"))
        briefing = report["campaign_briefing"]
        self.assertTrue(briefing["accepted"], briefing)
        audio = json.loads((directory / "tactical.json").read_text(encoding="utf-8"))["battle_audio"]
        self.assertTrue(any(row["event"] == "EAWR_TOAST_SPEECH" for row in audio["announcement_starts"]),
                        "the authored speech must actually start even when the movie fails")
        # Closing destroys the tactical owner before the final galaxy report.
        # Observe live stream/node state immediately before that teardown.
        proof = json.loads((directory / "driver.json").read_text())
        self.assertTrue(proof["speech_playing"], proof)
        self.assertNotIn("SCRIPT ERROR", result.stdout + result.stderr)
        if variant == "valid":
            self.assertTrue(proof["warmup_seen"], proof)
            self.assertGreater(proof["warmup_alpha"], 0.007, proof)
            self.assertGreaterEqual(proof["warmup_draws"], 2, proof)
            self.assertGreaterEqual(proof["first_battle_frame"], 0, proof)
            self.assertLessEqual(proof["warmup_end_frame"], proof["first_battle_frame"], proof)
            self.assertTrue(all(row["loading"] for row in proof["warmup_trace"] if row["warming"]), proof)
            self.assertGreater(proof["frames"], 1, proof)
            self.assertTrue(proof["paused"] and proof["resumed"], proof)
            self.assertLess(proof["pause_drift"], 0.05, proof)
            x, y, w, h = proof["rect"]
            self.assertTrue(0 <= x < 1280 and 0 <= y < 720 and w > 0 and h > 0, proof)
        else:
            self.assertFalse(proof["movie_present"], proof)
        print("GC hologram movie evidence:", directory)

    def test_authored_movie_pause_and_failures(self):
        for variant in ("valid", "missing", "undecodable"):
            with self.subTest(variant=variant):
                self.run_movie(variant)
