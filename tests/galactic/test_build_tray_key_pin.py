"""GBT-07: the build tray key's refusal set covers every key the galaxy view handles itself.

The tray key is one key without modifiers. Every unmodified key the galactic sources act on must be in
presentation::ui::build_tray_reserved_keys (settings.hpp), so a new galaxy key cannot silently share the tray's
binding. A key the galaxy view only uses with Ctrl (Ctrl+F, the planet search) or that the engine names with a space
(the keypad Enter, which the binding rule refuses anyway) is listed below with its reason.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SETTINGS = ROOT / "include/eawr/presentation/ui/settings.hpp"
GALACTIC = ROOT / "apps/viewer/src/galactic"

# Engine constant -> the engine's key name, for the constants the galactic sources use.
NAMES = {"ESCAPE": "Escape", "SPACE": "Space", "HOME": "Home", "PAGEUP": "PageUp", "PAGEDOWN": "PageDown",
         "UP": "Up", "DOWN": "Down", "LEFT": "Left", "RIGHT": "Right", "ENTER": "Enter"}
NOT_BINDING = {
    "NONE": "not a key",
    "CTRL": "a modifier; the tray key has none",
    "F": "only used as Ctrl+F (the planet search); a bare F is free",
    "KP_ENTER": "named 'Kp Enter'; a name with a space is refused by the binding rule",
    "TAB": "the tray key's own default",
}


def reserved_keys():
    text = SETTINGS.read_text(encoding="utf-8")
    block = re.search(r"build_tray_reserved_keys\{(.*?)\};", text, re.S)
    return set(re.findall(r'"([^"]+)"', block.group(1)))


def galactic_keys():
    found = set()
    for path in sorted(GALACTIC.glob("*.cpp")):
        found.update(re.findall(r"\bKEY_([A-Z0-9_]+)\b", path.read_text(encoding="utf-8")))
    return found


class BuildTrayKeyPin(unittest.TestCase):
    def test_every_galaxy_key_is_refused_for_the_tray(self):
        reserved = reserved_keys()
        self.assertTrue(reserved, "the reserved list is parsed")
        keys = galactic_keys()
        # Sentinels: the scan must find keys the galaxy view is known to handle, so it can never pass on nothing.
        self.assertIn("ESCAPE", keys, "the scan finds the galaxy view's Escape")
        self.assertIn("F3", keys, "the scan finds the performance overlay key")
        missing = []
        for constant in sorted(keys):
            if constant in NOT_BINDING:
                continue
            name = NAMES.get(constant, constant if re.fullmatch(r"[A-Z]|F[0-9]+", constant) else None)
            self.assertIsNotNone(name, f"KEY_{constant}: add its engine name to NAMES or a reason to NOT_BINDING")
            if name not in reserved:
                missing.append(name)
        self.assertEqual(missing, [], "galaxy keys the tray binding could take; add them to build_tray_reserved_keys")


if __name__ == "__main__":
    unittest.main()
