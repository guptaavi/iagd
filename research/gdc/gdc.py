"""Reader for Grim Dawn's player.gdc character saves - READ ONLY, never writes.

The file is obfuscated with a rolling XOR. The first four bytes hold a seed; every value read
advances the key, so decoding is value-aware rather than a flat byte stream:

    int    : value = raw_u32 ^ key,           then the key advances over its 4 raw bytes
    byte   : value = raw_byte ^ (key & 0xFF), then the key advances over that byte
    string : an int length, then that many bytes

Decoding the file as a flat byte stream produces noise, because an int XORs against the whole
32-bit key while a string byte XORs against the low byte only - get the granularity wrong and
everything after the first value desyncs.
"""

import struct


class Decoder:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()

        self.pos = 4
        seed = struct.unpack_from("<I", self.data, 0)[0] ^ 0x55555555
        self.key = seed

        self.table = []
        k = seed
        for _ in range(256):
            k = ((k >> 1) | (k << 31)) & 0xFFFFFFFF
            k = (k * 39916801) & 0xFFFFFFFF
            self.table.append(k)

    def _advance(self, raw_bytes):
        for b in raw_bytes:
            self.key ^= self.table[b]

    def read_int(self):
        raw = self.data[self.pos:self.pos + 4]
        self.pos += 4
        value = struct.unpack("<I", raw)[0] ^ self.key
        self._advance(raw)
        return value

    def peek_int(self):
        """The next int WITHOUT consuming it or advancing the key."""
        raw = self.data[self.pos:self.pos + 4]
        return struct.unpack("<I", raw)[0] ^ self.key

    def read_byte(self):
        raw = self.data[self.pos]
        self.pos += 1
        value = raw ^ (self.key & 0xFF)
        self._advance([raw])
        return value

    def read_string(self):
        """ASCII string: one byte per character. Used for record paths."""
        length = self.read_int()
        if length > 4096:
            raise ValueError(f"implausible string length {length} at {self.pos}")

        return "".join(chr(self.read_byte()) for _ in range(length))

    def read_wstring(self):
        """UTF-16 string: the length is a CHARACTER count, two bytes each. Used for names."""
        length = self.read_int()
        if length > 4096:
            raise ValueError(f"implausible wstring length {length} at {self.pos}")

        chars = []
        for _ in range(length):
            low = self.read_byte()
            high = self.read_byte()
            chars.append(chr(low | (high << 8)))

        return "".join(chars)

    def mark(self):
        """Snapshot (position, key) so a speculative read can be undone - the key advances on every
        read, so there is no random access and probing a layout needs explicit backtracking."""
        return self.pos, self.key

    def reset(self, state):
        self.pos, self.key = state

    def read_block_start(self):
        """Blocks are (type, length); the length is stored UNencrypted-adjusted by the key."""
        block_type = self.read_int()
        raw_len = struct.unpack_from("<I", self.data, self.pos)[0]
        self.pos += 4
        length = raw_len ^ self.key
        return block_type, length
