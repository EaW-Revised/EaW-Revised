"""Synthetic load-note JSON/listing/provenance contract; no retail assets."""
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile


def main():
    helper = Path(sys.argv[1]).resolve()
    import mods_service_contract as contract
    contract.SERVICE = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        game, mod, user = root / "game", root / "mod", root / "user"
        for data in (game / "GameData/Data", game / "corruption/Data", mod / "Data"):
            (data / "XML").mkdir(parents=True)
            (data / "MegaFiles.xml").write_text("<Mega_Files><File>fixture.meg</File></Mega_Files>")
            name, payload = b"Data/XML/fixture.xml", b"<Fixture/>"
            archive = struct.pack("<IIH", 1, 1, len(name)) + name
            archive += struct.pack("<IIIII", 0x12345678, 0, len(payload), 8 + 2 + len(name) + 20, 0) + payload
            (data / "fixture.meg").write_bytes(archive)
        registries = {"GameObjectFiles": "Game_Object_Files", "HardPointDataFiles": "Hard_Point_Files",
                      "FactionFiles": "Faction_Files", "CampaignFiles": "Campaign_Files", "SFXEventFiles": "SFXEvent_Files"}
        for file, tag in registries.items():
            (game / "corruption/Data/XML" / (file + ".xml")).write_text(f"<{tag}/>")
        registry = mod / "Data/XML/GameObjectFiles.xml"
        registry.write_text('<Game_Object_Files>\n<File> missing-one.xml </File>\n<File>missing-two.xml</File>\n</Game_Object_Files>')
        (game / "corruption/StarWarsG.exe").write_bytes(b"")
        profile = root / "profile.json"
        profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1, "id": "synthetic",
                                       "name": "synthetic", "layers": [{"key": "manual:fixture", "root": str(mod)}]}))
        user.mkdir()
        content_before = {str(p): p.read_bytes() for folder in (game, mod) for p in folder.rglob("*") if p.is_file()}
        args = [str(helper), str(profile), str(user), str(user / "report.json"), str(game)]
        def run(flags):
            result = subprocess.run(args + flags, capture_output=True, text=True, encoding="utf-8", timeout=60)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            return result
        normal = run([])
        normal_report = json.loads(normal.stdout.splitlines()[-1])["report"]
        assert normal_report["stage"] == "types", normal_report
        listed = run(["--notes", "--json"])
        rows = [json.loads(line) for line in listed.stdout.splitlines() if line]
        envelope = rows[-1]
        assert envelope["schema_version"] == 1, envelope
        print("loadcheck --notes --json sample: " + json.dumps(envelope, ensure_ascii=False))
        assert len(envelope["groups"]) == 1, envelope
        group = envelope["groups"][0]
        assert group["layer"] == {"mod_id": "manual:fixture", "chain_position": 0}, group
        assert group["file"] == "data/xml/gameobjectfiles.xml", group
        assert len(group["notes"]) == 2, group
        expected = {"code", "class", "layer", "file", "line", "object", "tag", "authored_value", "action", "effective_value", "retail", "phase", "diagnostic_code", "message"}
        for note in group["notes"]:
            assert set(note) == expected and note["code"] == "EAWR-LOAD-0001", note
            assert note["class"] == "B" and note["action"] == "accepted" and "R-10" in note["retail"], note
            assert note["line"] in (2, 3) and note["tag"] == "File", note
            expected_object = "data/xml/missing-" + ("one" if note["line"] == 2 else "two") + ".xml"
            assert note["object"] == expected_object and note["effective_value"] == "omitted", note
            assert note["phase"] == "load" and note["diagnostic_code"] == "EAWR-XML-0004", note
            assert note["message"] == "active registry include is unavailable: " + expected_object, note
        assert group["notes"][0]["authored_value"] == " missing-one.xml ", group
        # Listing and persistence do not change the existing admission/report contract.
        assert rows[-2]["report"] == normal_report
        assert envelope["profile_id"] == "synthetic" and len(envelope["chain_fingerprint"]) == 64
        client = contract.Interactive(args=["--user-data", str(user)])
        try:
            client.send(contract.hello(1, min_minor=1)); assert "result" in client.receive()
            client.send(contract.request(2, "game.find", hint=str(game))); assert "result" in client.receive()
            client.send(contract.request(3, "profile.save", profile=json.loads(profile.read_text())))
            assert "result" in client.receive()
            client.send(contract.request(4, "loadcheck.result", profile="synthetic"))
            service_result = client.receive()
            assert service_result["result"]["chain_fingerprint"] == envelope["chain_fingerprint"], service_result
        finally:
            client.close(); assert client.wait() == 0
        assert envelope["written_at"].endswith("Z") and envelope["game_version"]
        assert envelope["totals"] == {"A": 0, "B": 2, "C": 0} and not envelope["truncated"]
        assert group["totals"] == envelope["totals"] and not group["truncated"]
        paths = list((user / "mods/cache/loadcheck").glob("last-*/load-notes.json"))
        assert len(paths) == 1 and all(json.loads(p.read_text()) == envelope for p in paths), paths
        text = run(["--notes"]).stdout
        assert "manual:fixture / data/xml/gameobjectfiles.xml" in text and "EAWR-LOAD-0001 [warning]" in text
        assert "EAWR-LOAD-0001 [warning]" in listed.stderr
        assert content_before == {str(p): p.read_bytes() for folder in (game, mod) for p in folder.rglob("*") if p.is_file()}
        registry.write_text("<Game_Object_Files/>")
        empty = run(["--notes", "--json"])
        assert json.loads(empty.stdout.splitlines()[-1])["groups"] == []
    print("load-note contract passed: grouped JSON, log, sessions, authoring, unchanged content")


if __name__ == "__main__":
    main()
