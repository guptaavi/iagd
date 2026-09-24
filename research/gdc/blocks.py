"""Block framing for Grim Dawn saves - READ ONLY.

Framing (per xaviershay/gd-explorer, which matches what the files do here):

    id       decoded int, ADVANCES the cipher state
    length   raw u32 XOR state, does NOT advance
    body     `length` bytes, advances
    checksum raw u32, does NOT advance, and must EQUAL the state after the body

That last rule is what makes this worth doing properly: a parser that mishandles the cipher
desyncs silently, and a desynced stream still yields plausible-looking integers. The checksum
turns "looks reasonable" into "verified" - if it matches after skipping a block byte by byte,
the cipher handling is right.
"""

import struct

from gdc import Decoder


def read_length(d):
    """Length is XORed with the state but does not advance it."""
    raw = struct.unpack_from("<I", d.data, d.pos)[0]
    d.pos += 4
    return raw ^ d.key


def read_checksum(d):
    """Checksum is raw and does not advance the state."""
    raw = struct.unpack_from("<I", d.data, d.pos)[0]
    d.pos += 4
    return raw


def skip_body(d, length):
    """Advance over a body we do not parse, keeping the cipher state in step."""
    for _ in range(length):
        d.read_byte()


def iter_blocks(d, limit=64):
    """Yields (block_id, length, body_start, checksum_ok) walking the block stream."""
    blocks = []
    while d.pos < len(d.data) - 8 and len(blocks) < limit:
        block_id = d.read_int()
        length = read_length(d)
        body_start = d.pos

        if length < 0 or body_start + length > len(d.data):
            blocks.append((block_id, length, body_start, False))
            break

        skip_body(d, length)
        checksum = read_checksum(d)
        blocks.append((block_id, length, body_start, checksum == d.key))

    return blocks
