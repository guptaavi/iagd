# Research spikes

Standalone Python used to work out Grim Dawn's file formats before porting anything to C#.
Not part of the mod build. Nothing here writes to game files.

## `arz/` — database.arz reader (COMPLETE, ported to C#)

Reads every record from `database/database.arz`. Two things that cost time:

- Records are **LZ4 block** compressed, not zlib, at `offset + 24`.
- IAGD's own `ArzParser.IsInteresting` deliberately drops `/loottables/` and `/lootchests/`,
  which is exactly what the loot graph needs — hence a separate reader.

`lootgraph.py` walks creature → loot table → item transitively and builds the reverse index.
Numbers on the shipped game data: 1691 tables, 1357 creatures with loot, 1323 items with a
named source, 359 craftable. The C# port (`IAGrim/Parsers/Arz/`) reproduces these exactly,
which is how the port was validated.

## `gdc/` — character save reader (INCOMPLETE)

### What works, verified

- The cipher: seed = `first_u32 ^ 0x55555555`, a 256-entry table built with
  `v = ((v << 31) | (v >> 1)) * 39916801 mod 2^32`, state advanced per **ciphertext** byte.
  `player.gdc` decodes to magic `GDCX`.
- Value-aware decoding. This is the trap: an int XORs against the whole 32-bit state, a string
  byte against the low byte only. Decoding the file as a flat byte stream yields noise.
- Strings come in two kinds: ASCII (1 byte/char, record paths) and UTF-16 (2 bytes/char, names).
  The length is a CHARACTER count in both cases.
- Header fields, across all four save folders: name, sex, class tag, level, hardcore flag.
  e.g. `_Skeletor` -> name=Skeletor level=55 class=tagSkillClassName0508 hardcore=0.
- `transfer.gst` uses the same cipher: its first block reads as type 18, length 1688, which is
  exactly the remaining byte count.

### Where it stops

Block framing does not verify. Per gd-explorer the framing is

    id        decoded int, advances state
    length    raw u32 XOR state, does NOT advance
    body      length bytes, advances
    checksum  raw u32, does NOT advance, must EQUAL state after the body

That checksum should make the parser self-verifying, but after the header fields above, no
block validates — every byte offset from 0 to 95 past them was tried. So the header holds
fields not yet identified.

**Speculative parsing cannot recover from this.** One misread desyncs the stream permanently,
and a desynced stream still produces plausible-looking integers — an earlier scan reported zero
item records in a character that certainly has equipment, which looked like an empty file but
was a desynced one. Treat "the numbers look reasonable" as meaningless here; the checksum is
the only trustworthy signal.

### To resume

Port the layout from a known-good implementation rather than deducing it:

- <https://github.com/xaviershay/gd-explorer> (Haskell) — clearest description of the framing
- <https://github.com/AaronHutchinson/Grim-Dawn-Save-Decryption> (C++) — inventory block detail
- <https://github.com/ChrisElison/GDParser> (C#) — already borrows from Item Assistant

Get every block's checksum verifying BEFORE trusting a single item it reports. Then block 3
(inventory) holds sacks + equipment, each item being: basename, prefix, suffix, modifier,
transmute, seed, relic name, relic bonus, relic seed, augment name, unknown, augment seed,
relic completion level, stack count, plus position.

### Intended use

Character picker (hiding duplicate saves), equipped-items view, and a side-by-side stat diff
against a browsed item — raw numbers, no "better" verdict, since that depends on build,
resistances and caps.

**Reading only.** Writing `player.gdc` risks a character for no capability the shared stash does
not already provide, and moving items between characters already works through that stash.
A stash *deposit* with the game closed would need a `transfer.gst` writer, and that needs a
byte-identical round-trip proof first.

## Data files

Deliberately not committed: the game's `database.arz` (copyrighted game data) and the save
files used for testing (`player.gdc`, `transfer.gst` — personal data). Copy your own in.
