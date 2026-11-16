"""WST-14 / BA-55: the cloak handler's own activation cue is never played.

The original's stealth handler requests no sound on switch-on, service or termination, so the authored
`SFXEvent_Activate` of a nested stealth ability (the stock Phantom authors one, the Vengeance leaves it blank)
must stay silent, and a mod that authors it gets the same silence. This guard keeps the loader and the viewer
binding from growing a consumer for it, and keeps the registry rows and the behaviour notes aligned with that.
"""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
TAG = 'Abilities/Stealth_Ability/SFXEvent_Activate'


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def function_body(text, signature):
    begin = text.index(signature)
    opening = text.index('{', begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


class StealthActivateSilent(unittest.TestCase):
    def test_loader_does_not_read_the_cue(self):
        body = function_body(read('src/units/unit_tables_abilities_special.cpp'), 'void Loader::load_special_stealth(')
        self.assertNotIn('SFXEvent_Activate', body)
        self.assertNotRegex(body, r'activate_sfx')

    def test_viewer_binding_skips_stealth(self):
        text = read('apps/viewer/src/battle_audio_prepare.cpp')
        for match in re.finditer(r'SFXEvent_Activate', text):
            window = text[max(0, match.start() - 600):match.end() + 600]
            self.assertNotIn('SpecialAbilityKind::stealth', window)

    def test_registry_rows_are_sourced_foc_ignores(self):
        rows, evidence = [], {}
        for shard in (ROOT / 'docs/tag-coverage').glob('statuses*.json'):
            data = json.loads(shard.read_text(encoding='utf-8'))
            evidence.update(data.get('evidence', {}))
            rows += [r for r in data.get('rows', []) if r.get('tag') == TAG]
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(row['status'], 'foc-ignores', row)
            self.assertIn('debug build', evidence.get(row['evidence'], row['evidence']), row)

    def test_notes_cite_the_trace(self):
        self.assertIn('EST-18', read('docs/behaviour/walks/abilities.md'))
        audio = read('docs/behaviour/battle-audio.md')
        self.assertIn('never played', audio)
        self.assertNotIn('stays deferred', audio)


if __name__ == '__main__':
    unittest.main()
