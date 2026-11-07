"""Continuous audio starts reuse one cached loop stream per sample instead of duplicating it per start."""
from pathlib import Path
import re
import unittest

SRC = Path(__file__).resolve().parents[2] / "apps/viewer/src"


class AudioLoopStreamCacheTests(unittest.TestCase):
    def test_only_the_cache_builder_duplicates_the_stream(self):
        events = (SRC / "battle_audio_events.cpp").read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"stream->duplicate\(\)", events)), 1)
        builder = events[events.index("BattleAudio::loop_stream"):events.index("audio::Start BattleAudio::start_sample")]
        self.assertIn("stream->duplicate()", builder)
        self.assertIn("sample.loop.is_valid()", builder)
        self.assertIn("LOOP_FORWARD", builder)
        self.assertIn("set_loop_begin(0)", builder)

    def test_both_voice_paths_use_the_cached_stream(self):
        events = (SRC / "battle_audio_events.cpp").read_text(encoding="utf-8")
        start = events[events.index("audio::Start BattleAudio::start_sample"):]
        self.assertEqual(start.count("loop_stream(loaded)"), 2)
        self.assertIn("mutable godot::Ref<godot::AudioStreamWAV> loop;", (SRC / "battle_audio.hpp").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
