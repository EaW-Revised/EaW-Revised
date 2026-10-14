"""Runtime-only stream bytes: generated PCM and legally installed MP3, never repository assets."""
import io
import wave
import xml.etree.ElementTree as ET


def reversed_extensions(test, corpus, data_root):
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as stream:
        stream.setnchannels(1)
        stream.setsampwidth(2)
        stream.setframerate(22050)
        stream.writeframes(b"\x00\x00" * 11025)
    events = corpus.read_effective("foc", "data/xml/musicevents.xml")
    test.assertIsNotNone(events)
    names = [name for node in ET.fromstring(events.data) for name in (node.findtext("Files") or "").split()
             if name.lower().endswith(".mp3")]
    test.assertTrue(names)
    mp3 = corpus.read_effective("foc", "data/audio/music/" + names[0])
    test.assertIsNotNone(mp3)
    files = {"wave-content.mp3": buffer.getvalue(), "mp3-content.wav": mp3.data}
    for folder in ("Audio/Music", "Audio/Speech/English"):
        target = data_root / folder
        target.mkdir(parents=True, exist_ok=True)
        for name, content in files.items():
            (target / name).write_bytes(content)
    return files
