"""Fixed setup header handling for synthetic capture replay witnesses."""

import struct


def canonical_replay_header(recorded, version):
    """Return a v2/v3 witness header and the original setup-table offset.

    Native v6/v7 widen the header length by two bytes. Witnesses replace the
    recorded setup, so discard its tagged metadata and read its tables after
    the declared source header rather than after the fixed prefix.
    """
    header = bytearray(recorded[:104])
    source_version, size = struct.unpack_from("<HH", header, 8)
    wide = source_version in (6, 7)
    shift = 2 if wide else 0
    if wide:
        size = struct.unpack_from("<I", recorded, 10)[0]
        header = bytearray(recorded[:10] + b"\0\0" + recorded[14:106])
    assert source_version in (2, 3, 4, 5, 6, 7) and 104 + shift <= size <= len(recorded)
    assert source_version >= 4 or size == 104
    if source_version >= 4:
        assert size >= 108 + shift
        count = struct.unpack_from("<I", recorded, 104 + shift)[0]
        offset = 108 + shift
        previous_tag = 0
        for _ in range(count):
            assert offset + 4 <= size
            tag, length = struct.unpack_from("<HH", recorded, offset)
            assert tag > previous_tag and length > 0
            offset += 4
            if wide and tag == 15:
                assert offset + 2 <= size
                length = struct.unpack_from("<I", recorded, offset - 2)[0]
                offset += 2
            offset += length
            assert offset <= size
            previous_tag = tag
        assert offset == size
    assert version in (2, 3)
    struct.pack_into("<HH", header, 8, version, 104)
    return header, size
