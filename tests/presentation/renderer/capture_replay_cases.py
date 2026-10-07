"""Fixed setup header handling for synthetic capture replay witnesses."""

import struct


def canonical_replay_header(recorded, version):
    """Return a v2/v3 witness header and the original setup-table offset.

    Tagged v4/v5 retain the first 104 bytes of v2/v3. Witnesses replace the
    recorded setup, so discard its tagged metadata and read its tables after
    the declared source header rather than after the fixed prefix.
    """
    header = bytearray(recorded[:104])
    source_version, size = struct.unpack_from("<HH", header, 8)
    assert source_version in (2, 3, 4, 5) and 104 <= size <= len(recorded)
    assert source_version >= 4 or size == 104
    if source_version >= 4:
        assert size >= 108
        count = struct.unpack_from("<I", recorded, 104)[0]
        offset = 108
        previous_tag = 0
        for _ in range(count):
            assert offset + 4 <= size
            tag, length = struct.unpack_from("<HH", recorded, offset)
            assert tag > previous_tag and length > 0
            offset += 4 + length
            assert offset <= size
            previous_tag = tag
        assert offset == size
    assert version in (2, 3)
    struct.pack_into("<HH", header, 8, version, 104)
    return header, size
