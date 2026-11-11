"""Independent v2 content digest vector; requires python-xxhash 3.6.0."""
import re
import sys
from pathlib import Path
try:
    import xxhash
except ImportError:
    print("python-xxhash is not installed; skipping vector")
    sys.exit(77)

files = [("a.txt", b"alpha"), ("Sub/b.bin", bytes([0, 1, 2]) * 10), ("z.txt", b"zed")]
material = b"eawr-mod-content-xxh3-v2\n"
for path, content in sorted(files, key=lambda item: item[0].lower()):
    for field in (path.lower().encode(), str(len(content)).encode(), xxhash.xxh3_128_hexdigest(content).encode()):
        material += str(len(field)).encode() + b":" + field
actual = xxhash.xxh3_128_hexdigest(material, seed=0) + xxhash.xxh3_128_hexdigest(material, seed=1)
expected = re.search(r'kTinyDigest = "([0-9a-f]+)"', Path(sys.argv[1]).read_text()).group(1)
assert actual == expected, (actual, expected)
print(actual)
