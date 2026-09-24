"""Minimal reader for Grim Dawn's database.arz.

Layout mirrors IAGD's own parser (iagd/Parser/Arz/ArzParser.cs), which is the reference used here:

  header      : u16 unknown, u16 version, u32 recordTableStart, u32 recordTableSize,
                u32 recordTableEntryCount, u32 stringTableStart, u32 stringTableSize
  string table: repeated [u32 count, count * (u32 len, bytes)] until the table's byte length is consumed
  record entry: u32 stringIndex (the record's path), string type, u32 offset, u32 compressedSize,
                u32 uncompressedSize, 8 bytes skipped (timestamp)
  record data : LZ4 block at (offset + 24), decompressing to a run of fields:
                u16 type, u16 count, u32 keyStringIndex, then count * 4-byte values
                type 1 = float, type 2 = string index, anything else = int

Unlike IAGD this keeps EVERY record, not just items: loot tables and creatures are exactly what IAGD's
IsInteresting() filter throws away ("/loottables/" and "/lootchests/" are on its exclusion list).
"""

import struct
from collections import defaultdict

import lz4.block


class Arz:
    def __init__(self, path):
        with open(path, "rb") as handle:
            self.data = handle.read()

        (_unknown, _version, rec_start, _rec_size, rec_count,
         str_start, str_size) = struct.unpack_from("<HHIIIII", self.data, 0)

        self.strings = self._read_string_table(str_start, str_size)
        self.records = self._read_record_table(rec_start, rec_count)

    def _read_string_table(self, start, size):
        strings = []
        pos, end = start, start + size
        while pos < end:
            (count,) = struct.unpack_from("<I", self.data, pos)
            pos += 4
            for _ in range(count):
                (length,) = struct.unpack_from("<I", self.data, pos)
                pos += 4
                strings.append(self.data[pos:pos + length].decode("latin-1"))
                pos += length
        return strings

    def _read_record_table(self, start, count):
        records, pos = [], start
        for _ in range(count):
            (name_idx,) = struct.unpack_from("<I", self.data, pos)
            pos += 4
            (type_len,) = struct.unpack_from("<I", self.data, pos)
            pos += 4 + type_len
            offset, comp_size, uncomp_size = struct.unpack_from("<III", self.data, pos)
            pos += 12 + 8  # + timestamp
            records.append((self.strings[name_idx], offset, comp_size, uncomp_size))
        return records

    def fields(self, record):
        """Decode one record into {key: [values]}."""
        _name, offset, comp_size, _uncomp = record
        # LZ4 block, not zlib - IAGD calls LZ4.LZ4Codec.Decode here and ships LZ4.dll for it. The block
        # format carries no size header, so the uncompressed length from the record entry is required.
        raw = lz4.block.decompress(
            self.data[offset + 24: offset + 24 + comp_size], uncompressed_size=_uncomp)

        out = defaultdict(list)
        pos = 0
        while pos + 8 <= len(raw):
            ftype, count, key_idx = struct.unpack_from("<HHI", raw, pos)
            key = self.strings[key_idx]
            pos += 8
            for _ in range(count):
                if ftype == 1:
                    (value,) = struct.unpack_from("<f", raw, pos)
                elif ftype == 2:
                    (idx,) = struct.unpack_from("<I", raw, pos)
                    value = self.strings[idx]
                else:
                    (value,) = struct.unpack_from("<I", raw, pos)
                out[key].append(value)
                pos += 4
        return out

    def by_prefix(self, *prefixes):
        for record in self.records:
            if record[0].startswith(prefixes):
                yield record
