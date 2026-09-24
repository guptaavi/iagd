"""Reverse index: item record -> the enemies that can drop it.

The edges all live in database.arz:

    records/creatures/enemies/...   loot*Item*/loot*TableName* fields -> loot table records
    records/items/loottables/...    lootName*/lootTableName*/item*  -> item records or further tables

Loot tables nest (a creature's table points at other tables), so reaching the items means walking that
graph transitively. Weights (lootWeight*) are carried along but deliberately not turned into a drop
percentage here: the real chance also depends on the creature's own loot roll counts, difficulty scaling
and whether the table is a "dynamic" one that rolls a base item plus separate affixes. "Can drop" is
defensible from this data; "drops 0.4% of the time" is not.
"""

import sys
from collections import defaultdict

from arz import Arz

LOOT_KEY_HINTS = ("loot", "item")


def is_record(value):
    return isinstance(value, str) and value.endswith(".dbr")


def build(arz):
    """Returns (table_children, creature_tables) as record-path -> [record-path]."""
    table_children = {}
    creature_tables = defaultdict(list)

    for record in arz.records:
        name = record[0]
        is_table = name.startswith(("records/items/loottables/", "records/items/lootchests/"))
        is_creature = name.startswith("records/creatures/")

        if not (is_table or is_creature):
            continue

        try:
            fields = arz.fields(record)
        except Exception:
            continue

        refs = []
        for key, values in fields.items():
            if not any(hint in key.lower() for hint in LOOT_KEY_HINTS):
                continue
            refs.extend(v for v in values if is_record(v))

        if is_table:
            table_children[name] = refs
        elif refs:
            creature_tables[name] = refs

    return table_children, creature_tables


def sources_by_item(table_children, creature_tables):
    """item record -> set(creature records), following loot tables transitively."""
    item_sources = defaultdict(set)

    for creature, roots in creature_tables.items():
        seen, stack = set(), list(roots)
        while stack:
            node = stack.pop()
            if node in seen:
                continue
            seen.add(node)

            if node in table_children:
                stack.extend(table_children[node])
            elif node.startswith("records/items/"):
                item_sources[node].add(creature)

    return item_sources


def main():
    arz = Arz(sys.argv[1] if len(sys.argv) > 1 else "database.arz")
    table_children, creature_tables = build(arz)
    item_sources = sources_by_item(table_children, creature_tables)

    print(f"loot tables      : {len(table_children)}")
    print(f"creatures w/loot : {len(creature_tables)}")
    print(f"items with source: {len(item_sources)}")

    for probe in sys.argv[2:]:
        matches = [i for i in item_sources if probe in i]
        for item in matches[:3]:
            names = sorted(item_sources[item])
            print(f"\n{item}\n  {len(names)} source(s), e.g.:")
            for n in names[:6]:
                print(f"    {n}")


if __name__ == "__main__":
    main()
