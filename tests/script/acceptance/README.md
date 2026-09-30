# P0-07 independent acceptance fixtures

`test_p007_fixtures.py` writes small PGLua records from the approved fixed-width
contract and independently walks them.  It intentionally does not import the
production parser or reuse implementation serializers; malformed headers,
truncation/trailing bytes, string boundaries, signed counts, persistence IDs,
constant tags, and opcode limits are generated as synthetic adversarial cases.

Run from the repository root with:

```text
python -m unittest discover -s tests/script/acceptance -p "test_*.py" -v
```

Runtime and retail evidence are separate from this fixture-only suite.  Use the
normal `script_contracts` CTest and the pinned `script_smoke` command, recording
only safe metadata from the smoke report; never add extracted retail bytes to
this directory.
