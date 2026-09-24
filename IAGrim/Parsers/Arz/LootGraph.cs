using IAGrim.Parser.Arz;
using log4net;

namespace IAGrim.Parsers.Arz {

    /// <summary>
    /// "Dropped by" / "Crafted from" for an item, derived from the game's own database.arz.
    ///
    /// Three record families carry the edges, and IAGD's normal parse throws two of them away (its
    /// ArzParser.IsInteresting explicitly excludes "/loottables/" and "/lootchests/"):
    ///
    ///   records/creatures/...      loot*Item*/loot*TableName* -> loot table records
    ///   records/items/loottables/  lootName*/lootTableName*   -> item records, or further tables
    ///   records/items/crafting/    the blueprint's output item
    ///
    /// Tables nest, so reaching the items means walking that graph transitively.
    ///
    /// WHAT THIS DELIBERATELY DOES NOT DO: turn lootWeight* into a drop percentage. The real chance also
    /// depends on the creature's loot roll count, difficulty scaling, and whether the table rolls a base item
    /// with separately-rolled affixes. "Can drop" is supported by this data; a percentage is not.
    ///
    /// Most gear has no named source at all - Grim Dawn rolls it from level-appropriate dynamic pools, which
    /// is why GrimTools shows "Random drop" for the bulk of the database. Roughly 1300 of 34000 records
    /// resolve to a specific creature.
    /// </summary>
    public class LootGraph {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(LootGraph));

        private readonly Dictionary<string, List<string>> _sourcesByItem = new();
        private readonly Dictionary<string, List<string>> _blueprintsByItem = new();

        /// <summary>Record -> its description tag, for turning a source or blueprint into a display name.</summary>
        private readonly Dictionary<string, string> _creatureTags = new();

        public bool IsLoaded { get; private set; }

        public IReadOnlyList<string> SourcesFor(string? itemRecord) {
            if (string.IsNullOrEmpty(itemRecord) || !_sourcesByItem.TryGetValue(itemRecord!, out var sources)) {
                return Array.Empty<string>();
            }

            return sources;
        }

        public IReadOnlyList<string> BlueprintsFor(string? itemRecord) {
            if (string.IsNullOrEmpty(itemRecord) || !_blueprintsByItem.TryGetValue(itemRecord!, out var blueprints)) {
                return Array.Empty<string>();
            }

            return blueprints;
        }

        /// <summary>
        /// Faction vendors sell their stock through records under
        /// records/creatures/npcs/merchants/factiontables/, named "&lt;faction&gt;_&lt;tier&gt;_01". Those records
        /// carry no display name of their own - only marketStaticItems and a factiontier template - and the
        /// game's faction names live in the UI text resources, which IAGD does not parse (its ItemTag table
        /// holds item text only). So the seven base-game keys are mapped here explicitly; anything unmapped
        /// falls back to the key itself rather than inventing a name.
        /// </summary>
        private static readonly Dictionary<string, string> FactionNames = new(StringComparer.OrdinalIgnoreCase) {
            { "blacklegion", "The Black Legion" },
            { "devilscrossing", "Devil's Crossing" },
            { "homestead", "Homestead" },
            { "kymonchosen", "Kymon's Chosen" },
            { "orderdeathsvigil", "Order of Death's Vigil" },
            { "rovers", "The Rovers" },
            { "exile", "The Outcast" },
        };

        /// <summary>
        /// A readable label for a source record: a faction vendor becomes "Order of Death's Vigil (Honored)",
        /// a creature becomes its in-game name, and anything else falls back to the record's file name.
        /// </summary>
        public string SourceLabel(string record, IReadOnlyDictionary<string, string>? tags) {
            if (record.Contains("/merchants/factiontables/")) {
                var stem = Path.GetFileNameWithoutExtension(record);
                var parts = stem.TrimStart('_').Split('_');
                if (parts.Length >= 2) {
                    var faction = FactionNames.TryGetValue(parts[0], out var name) ? name : parts[0];
                    var tier = char.ToUpperInvariant(parts[1][0]) + parts[1][1..];
                    return $"{faction} ({tier})";
                }
            }

            var tag = TagFor(record);
            if (tag != null && tags != null && tags.TryGetValue(tag, out var display) && !string.IsNullOrWhiteSpace(display)) {
                return display;
            }

            return Path.GetFileNameWithoutExtension(record);
        }

        /// <summary>
        /// Item records whose source or blueprint label contains <paramref name="fragment"/>, so a search for
        /// "Death's Vigil" or a monster's name finds the items it can yield. Capped, because a short fragment
        /// can match most of the database and the caller binds these as query parameters.
        /// </summary>
        public IReadOnlyCollection<string> ItemsFromSourcesMatching(string fragment, IReadOnlyDictionary<string, string>? tags, int limit = 2000) {
            var matches = new HashSet<string>();
            if (!IsLoaded || string.IsNullOrWhiteSpace(fragment)) {
                return matches;
            }

            var labelCache = new Dictionary<string, bool>();

            bool Matches(string record) {
                if (!labelCache.TryGetValue(record, out var hit)) {
                    labelCache[record] = hit = SourceLabel(record, tags)
                        .Contains(fragment, StringComparison.OrdinalIgnoreCase);
                }

                return hit;
            }

            foreach (var (item, sources) in _sourcesByItem) {
                if (sources.Any(Matches) && matches.Add(item) && matches.Count >= limit) {
                    return matches;
                }
            }

            foreach (var (item, blueprints) in _blueprintsByItem) {
                if (blueprints.Any(Matches) && matches.Add(item) && matches.Count >= limit) {
                    return matches;
                }
            }

            return matches;
        }

        public string? TagFor(string creatureRecord) {
            return _creatureTags.TryGetValue(creatureRecord, out var tag) ? tag : null;
        }

        /// <summary>
        /// Reads database.arz and builds the reverse index. Takes a couple of seconds and allocates while it
        /// runs, so callers should do this off the UI thread; the maps are only published once complete.
        /// </summary>
        public void Load(string arzFile) {
            if (!File.Exists(arzFile)) {
                Logger.Warn($"No database.arz at {arzFile}; drop sources will be unavailable");
                return;
            }

            var started = System.Diagnostics.Stopwatch.StartNew();

            var tableChildren = new Dictionary<string, List<string>>();
            var creatureTables = new Dictionary<string, List<string>>();

            foreach (var record in ArzRecordReader.Read(arzFile)) {
                var isTable = record.Name.StartsWith("records/items/loottables/") ||
                              record.Name.StartsWith("records/items/lootchests/");
                var isCreature = record.Name.StartsWith("records/creatures/");
                var isBlueprint = record.Name.StartsWith("records/items/crafting/");

                if (!isTable && !isCreature && !isBlueprint) {
                    continue;
                }

                // A blueprint names its output in artifactName - no "loot" or "item" in the key, so it has to
                // be read explicitly rather than through the loot-field sweep below.
                if (isBlueprint) {
                    // Its own description tag, so the UI can print "Blueprint: Leviathan" rather than the
                    // record's file name.
                    if (record.Fields.TryGetValue("description", out var tag) && tag.Count > 0) {
                        _creatureTags[record.Name] = tag[0];
                    }

                    if (record.Fields.TryGetValue("artifactName", out var crafted)) {
                        foreach (var produced in crafted.Where(r => r.EndsWith(".dbr"))) {
                            Add(_blueprintsByItem, produced, record.Name);
                        }
                    }

                    continue;
                }

                var refs = new List<string>();
                foreach (var field in record.Fields) {
                    var key = field.Key.ToLowerInvariant();

                    if (isCreature && key == "description" && field.Value.Count > 0) {
                        _creatureTags[record.Name] = field.Value[0];
                        continue;
                    }

                    if (!key.Contains("loot") && !key.Contains("item")) {
                        continue;
                    }

                    refs.AddRange(field.Value.Where(v => v.EndsWith(".dbr")));
                }

                if (isTable) {
                    tableChildren[record.Name] = refs;
                }
                else if (refs.Count > 0) {
                    creatureTables[record.Name] = refs;
                }
            }

            foreach (var (creature, roots) in creatureTables) {
                foreach (var item in Reachable(roots, tableChildren)) {
                    Add(_sourcesByItem, item, creature);
                }
            }

            IsLoaded = true;
            Logger.Info($"Loot graph: {tableChildren.Count} tables, {creatureTables.Count} creatures, " +
                        $"{_sourcesByItem.Count} items with a named source, {_blueprintsByItem.Count} craftable " +
                        $"({started.ElapsedMilliseconds} ms)");
        }

        /// <summary>Walks the loot tables reachable from one creature's roots, yielding the item records.</summary>
        private static IEnumerable<string> Reachable(List<string> roots, Dictionary<string, List<string>> tableChildren) {
            var seen = new HashSet<string>();
            var stack = new Stack<string>(roots);

            while (stack.Count > 0) {
                var node = stack.Pop();
                if (!seen.Add(node)) {
                    continue;
                }

                if (tableChildren.TryGetValue(node, out var children)) {
                    foreach (var child in children) {
                        stack.Push(child);
                    }
                }
                else if (node.StartsWith("records/items/")) {
                    yield return node;
                }
            }
        }

        private static void Add(Dictionary<string, List<string>> map, string key, string value) {
            if (!map.TryGetValue(key, out var list)) {
                map[key] = list = new List<string>();
            }

            if (!list.Contains(value)) {
                list.Add(value);
            }
        }
    }
}
