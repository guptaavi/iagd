using LZ4;

namespace IAGrim.Parser.Arz {

    /// <summary>One record out of database.arz, with its fields left as raw strings.</summary>
    public class ArzRecord {
        public string Name { get; set; } = string.Empty;
        public Dictionary<string, List<string>> Fields { get; set; } = new();
    }

    /// <summary>
    /// Streams every record out of database.arz.
    ///
    /// The existing ArzParser reads the same file but is built for items: it drops whole record families
    /// (IsInteresting excludes "/loottables/" among others) and converts what survives into IItem/IItemStat.
    /// The loot graph needs the families it discards and only ever looks at record-path values, so this reads
    /// the container directly and hands back raw fields.
    ///
    /// Layout (same as ArzParser, which is the reference):
    ///   header      : u16 unknown, u16 version, u32 recordTableStart, u32 recordTableSize,
    ///                 u32 recordTableEntryCount, u32 stringTableStart, u32 stringTableSize
    ///   string table: repeated [u32 count, count * (u32 len, bytes)]
    ///   record entry: u32 stringIndex, string type, u32 offset, u32 compressedSize, u32 uncompressedSize,
    ///                 8 bytes skipped
    ///   record data : LZ4 BLOCK (not zlib) at offset + 24, then fields of
    ///                 u16 type, u16 count, u32 keyStringIndex, count * 4 bytes
    ///                 type 1 = float, type 2 = string index, otherwise int
    /// </summary>
    public static class ArzRecordReader {

        public static IEnumerable<ArzRecord> Read(string path) {
            var data = File.ReadAllBytes(path);

            var recordTableStart = BitConverter.ToUInt32(data, 4);
            var recordCount = BitConverter.ToUInt32(data, 12);
            var stringTableStart = BitConverter.ToUInt32(data, 16);
            var stringTableSize = BitConverter.ToUInt32(data, 20);

            var strings = ReadStringTable(data, stringTableStart, stringTableSize);

            var pos = (int)recordTableStart;
            for (var i = 0; i < recordCount; i++) {
                var nameIndex = BitConverter.ToUInt32(data, pos);
                pos += 4;

                var typeLength = BitConverter.ToInt32(data, pos);
                pos += 4 + typeLength;

                var offset = BitConverter.ToUInt32(data, pos);
                var compressed = BitConverter.ToInt32(data, pos + 4);
                var uncompressed = BitConverter.ToInt32(data, pos + 8);
                pos += 12 + 8; // + timestamp

                var record = Decode(data, strings, nameIndex, offset, compressed, uncompressed);
                if (record != null) {
                    yield return record;
                }
            }
        }

        private static ArzRecord? Decode(byte[] data, List<string> strings, uint nameIndex, uint offset, int compressed, int uncompressed) {
            byte[] raw;
            try {
                var payload = new byte[compressed];
                Array.Copy(data, offset + 24, payload, 0, compressed);
                raw = LZ4Codec.Decode(payload, 0, compressed, uncompressed);
            }
            catch {
                // A record we cannot decode costs one missing edge, not the whole index.
                return null;
            }

            var record = new ArzRecord { Name = strings[(int)nameIndex] };

            var pos = 0;
            while (pos + 8 <= raw.Length) {
                var type = BitConverter.ToUInt16(raw, pos);
                var count = BitConverter.ToUInt16(raw, pos + 2);
                var keyIndex = BitConverter.ToUInt32(raw, pos + 4);
                pos += 8;

                if (keyIndex >= strings.Count || pos + count * 4 > raw.Length) {
                    break;
                }

                var key = strings[(int)keyIndex];
                for (var n = 0; n < count; n++) {
                    // Only string values can name another record, which is all the loot graph follows.
                    if (type == 2) {
                        var valueIndex = BitConverter.ToUInt32(raw, pos);
                        if (valueIndex < strings.Count) {
                            if (!record.Fields.TryGetValue(key, out var values)) {
                                record.Fields[key] = values = new List<string>();
                            }

                            values.Add(strings[(int)valueIndex]);
                        }
                    }

                    pos += 4;
                }
            }

            return record;
        }

        private static List<string> ReadStringTable(byte[] data, uint start, uint size) {
            var strings = new List<string>();
            var pos = (int)start;
            var end = start + size;

            while (pos < end) {
                var count = BitConverter.ToUInt32(data, pos);
                pos += 4;

                for (var i = 0; i < count && pos < end; i++) {
                    var length = BitConverter.ToInt32(data, pos);
                    pos += 4;
                    strings.Add(System.Text.Encoding.Latin1.GetString(data, pos, length));
                    pos += length;
                }
            }

            return strings;
        }
    }
}
