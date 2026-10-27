"""Generate an independent native replay with a synthetic catalog larger than 64 KiB."""
from pathlib import Path
import struct

OUT = Path(__file__).resolve().parent


def generate():
    base = (OUT / "tactical-v2.eawr-replay").read_bytes()
    pack = lambda fmt, *values: struct.pack("<" + fmt, *values)
    text = lambda value: pack("H", len(value.encode())) + value.encode()
    players = struct.unpack_from("<I", base, 48)[0]
    slots = []
    for i in range(players):
        player, _, _, flags, _ = struct.unpack_from("<IIQII", base, 104 + i * 24)
        if flags & 1:
            slots.append(pack("IIII", player, int(player == 1), 0xFFFFFFFF, 0))
    metadata = (text("data/art/maps/renamed_campaign.ted") + text("b" * 64)
                + pack("I7q", 0, *([0] * 7)) + text("") + text("")
                + pack("II", 2, len(slots)) + b"".join(slots))
    units = struct.unpack_from("<Q", base, 56)[0]
    last_unit = struct.unpack_from("<Q", base, 104 + players * 24 + (units - 1) * 80)[0]
    roots = ["Mod_Carrier", "Mod_Station"] + [f"Synthetic_Catalog_Type_{i}" for i in range(3000)]
    native = pack("II", 1, 1)
    for names in (roots, ["Mod_Nebula"], ["Mod_Carrier", "Mod_Fighter"]):
        native += pack("I", len(names)) + b"".join(text(name) for name in names)
    extensions = (pack("HH", 2, len(metadata)) + metadata
                  + pack("HHQ", 6, 8, last_unit + 1)
                  + pack("HI", 15, len(native)) + native)
    header_size = 106 + 4 + len(extensions)
    return base[:8] + pack("HI", 6, header_size) + base[12:104] + pack("I", 3) + extensions + base[104:]


if __name__ == "__main__":
    (OUT / "native-large.bin").write_bytes(generate())
