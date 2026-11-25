import json
import pathlib
import unittest


class ConsoleRegistryTests(unittest.TestCase):
    def check_registry(self, registry):
        self.assertEqual(registry["blocks"], {"core": [1, 99], "battle": [100, 199],
                                               "galactic": [200, 299], "multiplayer": [300, 399]})
        seen = set()
        for verb in registry["verbs"]:
            number = verb["id"]
            low, high = registry["blocks"][verb["block"]]
            self.assertTrue(low <= number <= high)
            self.assertNotIn(number, seen)
            seen.add(number)

    def test_unique_allocated_ids_in_their_blocks(self):
        path = pathlib.Path(__file__).resolve().parents[2] / "docs/registries/console-verbs.json"
        self.check_registry(json.loads(path.read_text(encoding="utf-8")))

    def test_guard_rejects_duplicate_and_out_of_block_ids(self):
        blocks = {"core": [1, 99], "battle": [100, 199],
                  "galactic": [200, 299], "multiplayer": [300, 399]}
        for verbs in ([{"id": 1, "block": "core"}, {"id": 1, "block": "core"}],
                      [{"id": 100, "block": "core"}]):
            with self.assertRaises(AssertionError):
                self.check_registry({"blocks": blocks, "verbs": verbs})


if __name__ == "__main__":
    unittest.main()
