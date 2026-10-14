"""Resource-driven menu geometry and stock capture equivalence (GPU opt-in)."""

import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
DIALOGS = ("IDD_MAIN_MENU_DIALOG", "IDD_MAIN_OPTIONS_DIALOG", "IDD_SKIRMISH_STAGING_DIALOG")
# Authored decorations are allowed to cover their background; the empty scenario
# placeholder covers the preview by design. These are not separate interactive areas.
DECORATIONS = {"IDC_MENU_CREDITS", "IDC_PETROGLYPH_LOGO", "IDC_STATIC", "IDC_SCENARIO_DESCRIPTION_TEXT_BOX"}

DRIVER = r'''extends SceneTree
var evidence = ""
var snapshots = {}
var frames = 0
func _initialize():
    var args = OS.get_cmdline_user_args()
    for i in range(args.size() - 1):
        if args[i] == "--layout-evidence":
            evidence = args[i + 1]
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    var motion = InputEventMouseMotion.new()
    motion.position = Vector2.ZERO
    Input.parse_input_event(motion)
func rect_array(node):
    var rect = node.get_global_rect()
    return [rect.position.x, rect.position.y, rect.size.x, rect.size.y]
func scan(node):
    if node is Control and node.is_visible_in_tree() and str(node.name) in ["EawrSetupPreview", "EawrSetupPreviewFrame"]:
        snapshots[str(node.name)] = rect_array(node)
    if node is Control and str(node.name).begins_with("IDD_") and node.is_visible_in_tree():
        var controls = []
        for child in node.get_children():
            if child is Control and child.is_visible_in_tree():
                var id = str(child.name)
                # The baseline's project status label had no explicit name.
                if not id.begins_with("IDC_") and child.get_class() == "EawrUiLabel":
                    id = "EawrSetupStatus"
                if id.begins_with("IDC_") or id == "EawrSetupStatus":
                    controls.append({"id": id, "kind": child.get_class(), "rect": rect_array(child)})
        snapshots[str(node.name)] = {"frame": rect_array(node), "controls": controls}
    for child in node.get_children():
        scan(child)
func _process(_delta):
    frames += 1
    if frames > 4:
        scan(root)
        var file = FileAccess.open(evidence, FileAccess.WRITE)
        if file:
            file.store_string(JSON.stringify(snapshots))
    return false
'''


def intersection(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    return (min(ax + aw, bx + bw) - max(ax, bx),
            min(ay + ah, by + bh) - max(ay, by))


def resource_geometry(script):
    """Read numeric authored rectangles for this focused RC geometry probe.

    Unsupported expressions fail closed rather than silently allowing overlaps.
    The runtime catalogue remains the authority for building the actual dialogs.
    """
    script = re.sub(r'/\*.*?\*/|//[^\n]*', '', script, flags=re.S)
    number = r'(-?\d+)'
    # UI-RC1: repeated commas separate tokens, without adding empty fields.
    separator = r'(?:,\s*)+'
    rectangle = re.compile(separator + separator.join([number] * 4) + r'\s*(?:,|$)')
    dialogs = {}
    for name in DIALOGS:
        match = re.search(r'\b' + name + r'\s+DIALOG(?:EX)?\s+([^\n]+).*?\bBEGIN\b(.*?)\bEND\b',
                          script, re.S)
        if match is None:
            raise ValueError('missing resource dialog: ' + name)
        frame = [int(value.strip()) for value in match[1].split(',')]
        if len(frame) != 4 or min(frame[2:]) <= 0:
            raise ValueError('unsupported resource frame: ' + name)
        controls = {}
        statements = re.split(r'\n\s*(?=[A-Z]+\s)', match[2])
        for statement in statements:
            control = re.search(r'\b(IDC_\w+)\b', statement)
            if control is None:
                continue
            rect = rectangle.search(statement[control.end():].strip())
            if rect is None:
                raise ValueError('unsupported resource rectangle: ' + control[1])
            controls[control[1]] = list(map(int, rect.groups()))
            if min(controls[control[1]][2:]) <= 0:
                raise ValueError('unsupported resource size: ' + control[1])
        dialogs[name] = {'frame': frame, 'controls': controls}
    return dialogs


def loaded_resource_geometry(mod=False):
    from tools.inventory.corpus import Corpus
    corpus = Corpus(os.environ['EAWR_EAW_GAME_ROOT'], os.environ.get('EAWR_REMAKE_MOD_ROOT') if mod else None)
    source = corpus.read_effective('remake' if mod else 'foc', 'Data/Resources/GUIDialog/guidialogs.rc')
    if source is None:
        raise ValueError('missing loaded dialog resource')
    # The resource script is a text asset; support its Unicode BOMs as well as
    # the legacy byte encoding used by stock resources.
    codec = 'utf-16' if source.data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    try:
        script = source.data.decode(codec)
    except UnicodeDecodeError:
        script = source.data.decode('latin-1')
    return resource_geometry(script)


def geometry_errors(snapshot, authored=None):
    x, y, w, h = snapshot["frame"]
    controls = []
    errors = []
    for row in snapshot["controls"]:
        rx, ry, rw, rh = row["rect"]
        if row["kind"] == "EawrUiFrame" or row["id"] in DECORATIONS or rw <= 0 or rh <= 0:
            continue
        if rx < x - .01 or ry < y - .01 or rx + rw > x + w + .01 or ry + rh > y + h + .01:
            errors.append("outside dialog: " + row["id"])
        controls.append(row)
    for a, b in itertools.combinations(controls, 2):
        overlap_x, overlap_y = intersection(a['rect'], b['rect'])
        if overlap_x > .01 and overlap_y > .01:
            if authored and a['id'] in authored['controls'] and b['id'] in authored['controls']:
                authored_a, authored_b = authored['controls'][a['id']], authored['controls'][b['id']]
                allowed_x, allowed_y = intersection(authored_a, authored_b)
                # Compare penetration as a fraction of the smaller control in
                # each axis (buttons/closed combos may have theme-sized heights);
                # an authored pair is not a blanket exemption for deeper overlap.
                scale_x = min(a['rect'][2], b['rect'][2]) / min(authored_a[2], authored_b[2])
                scale_y = min(a['rect'][3], b['rect'][3]) / min(authored_a[3], authored_b[3])
                if (allowed_x > 0 and allowed_y > 0
                        and overlap_x <= allowed_x * scale_x + .01
                        and overlap_y <= allowed_y * scale_y + .01):
                    continue
            errors.append("overlap: " + a["id"] + " / " + b["id"])
    return errors


class MenuGeometryValidator(unittest.TestCase):
    def test_detects_bounds_and_overlap(self):
        snapshot = {"frame": [0, 0, 100, 100], "controls": [
            {"id": "first", "kind": "EawrUiButton", "rect": [10, 10, 30, 20]},
            {"id": "second", "kind": "EawrUiButton", "rect": [20, 10, 30, 20]},
            {"id": "outside", "kind": "EawrUiButton", "rect": [90, 50, 20, 20]}]}
        self.assertEqual(geometry_errors(snapshot), ["outside dialog: outside", "overlap: first / second"])

    def test_authored_overlap_is_bounded_and_other_pairs_still_fail(self):
        script = '\n'.join(name + ''' DIALOGEX 0, 0, 100, 100
BEGIN
    LISTBOX IDC_LIST,20,10,30,60,LBS_SORT
    PUSHBUTTON "tab",IDC_TAB,
               0,30,22,,12
    PUSHBUTTON "other",IDC_OTHER,60,30,20,12
END''' for name in DIALOGS)
        authored = resource_geometry(script)[DIALOGS[0]]
        snapshot = {'frame': [0, 0, 200, 150], 'controls': [
            {'id': 'IDC_LIST', 'kind': 'EawrUiList', 'rect': [40, 15, 60, 90]},
            {'id': 'IDC_TAB', 'kind': 'EawrUiButton', 'rect': [0, 45, 44, 24]},
            {'id': 'IDC_OTHER', 'kind': 'EawrUiButton', 'rect': [120, 45, 40, 24]}]}
        self.assertEqual(geometry_errors(snapshot, authored), [])
        snapshot['controls'][1]['rect'][0] = 1
        self.assertEqual(geometry_errors(snapshot, authored), ['overlap: IDC_LIST / IDC_TAB'])
        snapshot['controls'][1]['rect'][0] = 0
        snapshot['controls'][2]['rect'][0] = 90
        self.assertEqual(geometry_errors(snapshot, authored), ['overlap: IDC_LIST / IDC_OTHER'])

    def test_resource_expression_fails_closed(self):
        script = '\n'.join(name + ''' DIALOGEX 0, 0, 100, 100
BEGIN
    LISTBOX IDC_LIST,LEFT,10,30,60,LBS_SORT
END''' for name in DIALOGS)
        with self.assertRaisesRegex(ValueError, 'unsupported resource rectangle'):
            resource_geometry(script)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires the viewer GPU lane and installed game data")
class MenuModLayoutsGpu(unittest.TestCase):
    def test_stock_and_remake_layouts(self):
        self.check_layouts(include_remake=True)

    def test_stock_layouts_only(self):
        self.check_layouts(include_remake=False)

    def check_layouts(self, include_remake):
        from PIL import Image
        if include_remake:
            self.assertTrue(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "run this case with -NeedsMod")
        project = ROOT / "apps/viewer/project"
        dll = project / "bin/libeawr_viewer.windows.template_release.x86_64.dll"
        baseline = project / "common/menu-layout-baseline.dll"
        receipt = json.loads((project / "common/menu-layout-baseline.json").read_text())
        self.assertEqual(hashlib.sha256(baseline.read_bytes()).hexdigest(), receipt["sha256"])
        current = dll.read_bytes()
        with tempfile.TemporaryDirectory(prefix="menu-layouts-") as temporary:
            output = Path(temporary)
            driver = output / "probe.gd"
            driver.write_text(DRIVER, encoding="utf-8")
            def run(name, mod=False):
                authored = loaded_resource_geometry(mod)
                prefix = output / name
                evidence = output / (name + ".json")
                args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path", str(project),
                        "--script", str(driver), "--", "--eawr-front-end", "--eawr-front-end-test",
                        "--eawr-attract", "off", "--eawr-audio", "off", "--eawr-game-root",
                        os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-capture", str(prefix.with_suffix(".png")),
                        "--layout-evidence", str(evidence)]
                if mod:
                    args += ["--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"]]
                try:
                    result = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, encoding="utf-8", errors="replace", timeout=600)
                except subprocess.TimeoutExpired as error:
                    captured = error.stdout or ""
                    if isinstance(captured, bytes):
                        captured = captured.decode("utf-8", errors="replace")
                    (output / (name + ".log")).write_text(captured, encoding="utf-8")
                    raise
                (output / (name + ".log")).write_text(result.stdout, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout[-6000:])
                self.assertNotIn("using the project-authored fallback menu", result.stdout)
                self.assertNotIn("EAWR front end test:", result.stdout)
                snapshots = json.loads(evidence.read_text())
                for dialog in DIALOGS:
                    self.assertIn(dialog, snapshots)
                    self.assertEqual(geometry_errors(snapshots[dialog], authored[dialog]), [], dialog)
                self.assertEqual(sum(row["id"] == "EawrSetupStatus" for row in
                    snapshots["IDD_SKIRMISH_STAGING_DIALOG"]["controls"]), 1)
                return prefix, snapshots
            try:
                dll.write_bytes(baseline.read_bytes())
                before, before_geometry = run("baseline")
                dll.write_bytes(current)
                after, after_geometry = run("stock-\u0394\U0001f30c")
                # R-SETUP-01: stock image-slot placement at the canonical 1280x720
                # viewport, before the project's two-pixel image inset.
                self.assertEqual(after_geometry["EawrSetupPreviewFrame"], [58.125, 230.625, 237.5, 180.0])
                self.assertEqual(after_geometry["EawrSetupPreview"], [60.125, 232.625, 233.5, 176.0])
                # Collect the independent mod canary before stock identity can
                # abort the case, retaining captures for a fix-forward diagnosis.
                if include_remake:
                    _, remake = run("remake", mod=True)
                    self.assertNotEqual(after_geometry["IDD_SKIRMISH_STAGING_DIALOG"]["frame"],
                                        remake["IDD_SKIRMISH_STAGING_DIALOG"]["frame"])
                for dialog in DIALOGS:
                    before_controls = {row["id"]: row for row in before_geometry[dialog]["controls"]}
                    after_controls = {row["id"]: row for row in after_geometry[dialog]["controls"]}
                    self.assertEqual(before_geometry[dialog]["frame"], after_geometry[dialog]["frame"])
                    self.assertEqual(before_controls, after_controls, "stock global geometry changed: " + dialog)
                hashes = {}
                for label in ("main_menu", "options", "skirmish_setup"):
                    a = Image.open(str(before) + "." + label + ".png").convert("RGBA")
                    b = Image.open(str(after) + "." + label + ".png").convert("RGBA")
                    self.assertEqual(a.size, b.size)
                    self.assertEqual(a.tobytes(), b.tobytes(), "stock capture changed: " + label)
                    hashes[label] = hashlib.sha256(b.tobytes()).hexdigest()
                (output / "comparison.json").write_text(json.dumps({"baseline": receipt, "stockRgbaHashes": hashes,
                    "stockIdentical": True, "remakeDialogs": list(DIALOGS) if include_remake else []}, indent=2))
            finally:
                # The mod run already uses current bytes. Rewriting a loaded
                # Windows DLL after a timeout can mask the actual failure.
                if dll.read_bytes() != current:
                    dll.write_bytes(current)


if __name__ == "__main__":
    unittest.main()
