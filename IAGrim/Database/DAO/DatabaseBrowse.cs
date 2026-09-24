using log4net;
using NHibernate;
using NHibernate.Transform;

namespace IAGrim.Database.DAO {

    /// <summary>One row of the game's item database, as shown when browsing rather than searching the stash.</summary>
    public class DatabaseBrowseRow {
        public string BaseRecord { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;

        /// <summary>"Epic", "Legendary", ... - the game's own itemClassification.</summary>
        public string? Rarity { get; set; }

        /// <summary>Icon file name inside IAGD's storage folder, derived from the record's bitmap path.</summary>
        public string? Icon { get; set; }

        /// <summary>Record class, e.g. "ArmorProtective_Legs".</summary>
        public string? ItemClass { get; set; }

        public double Level { get; set; }
    }

    /// <summary>
    /// Searches every item Grim Dawn defines, not just the ones the player owns.
    ///
    /// IAGD already parses database.arz into DatabaseItem_v2/DatabaseItemStat_v2 to resolve names and stats -
    /// 9,639 named items - but the UI only ever queries PlayerItem, so that table is invisible to the user.
    /// This exposes it: the same grid, fed from the game database instead of the stash, which also means the
    /// set and "Obtained from" sections keep working because both are keyed on the base record.
    ///
    /// The stats pane is thinner for a database item than for an owned one: stat replicas are captured from
    /// the live game when an item is looted, so an item the player has never held has no tooltip to show.
    /// </summary>
    public class DatabaseBrowse {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(DatabaseBrowse));
        private readonly SessionFactory _sessionCreator;

        public DatabaseBrowse(SessionFactory sessionCreator) {
            _sessionCreator = sessionCreator;
        }

        /// <summary>
        /// IAGD's rarity names are NOT the game's itemClassification values, and the overlap is a trap: IAGD
        /// calls an Epic-class item "Blue" and reserves "Epic" for LEGENDARY-class items. Confirmed against
        /// the player's own items - every Blue is an Epic record, every Epic is a Legendary record:
        ///
        ///     Blue  -> Epic          Green -> Rare, and also Common (green is affix-driven, not base-driven)
        ///     Epic  -> Legendary     Yellow -> Magical
        ///
        /// Mapping "Epic" to "Epic" would filter for the wrong tier; leaving it unmapped, as an earlier version
        /// did, silently dropped the filter entirely and returned every rarity.
        /// </summary>
        private static readonly Dictionary<string, string> ClassificationByColour = new(StringComparer.OrdinalIgnoreCase) {
            { "Yellow", "Magical" },
            { "Green", "Rare" },
            { "Blue", "Epic" },
            { "Epic", "Legendary" },
            { "Purple", "Legendary" },
        };

        public static string ColourForClassification(string? classification) => classification?.ToLowerInvariant() switch {
            "magical" => "Yellow",
            "rare" => "Green",
            "epic" => "Blue",
            "legendary" => "Epic",
            "quest" => "Orange",
            "common" or "broken" => "White",
            _ => string.Empty,
        };

        /// <summary>
        /// Name search with the toolbar's rarity and level filters applied, capped so a blank query cannot
        /// try to render ten thousand rows at once.
        /// </summary>
        public IList<DatabaseBrowseRow> Search(string? nameFragment, string? rarityColour = null, double minLevel = 0,
            double maxLevel = 0, string[]? slotClasses = null, bool slotInverse = false,
            List<string[]>? statFilters = null, int limit = 500) {
            try {
                using ISession session = _sessionCreator.OpenSession();

                // The slot dropdown already speaks the database's own vocabulary: its Filter array holds
                // record Class values ("ArmorProtective_Head", "WeaponMelee_Dagger"), which is exactly what
                // the Class stat stores. No translation needed - only the EXISTS/NOT EXISTS choice, since
                // one entry ("other") is an inverse filter.
                var hasSlots = slotClasses is { Length: > 0 };
                var slotClause = hasSlots
                    ? $@" AND {(slotInverse ? "NOT EXISTS" : "EXISTS")} (SELECT 1 FROM DatabaseItemStat_v2 SL
                          WHERE SL.id_databaseitem = I.id_databaseitem AND SL.Stat = 'Class'
                          AND SL.TextValue IN (:slots))"
                    : string.Empty;

                // The left-hand panel's checkboxes: each ticked box contributes one array of stat names, and
                // the item must carry a stat from EVERY array - ticking Aether and Fire means both, which is
                // the same AND-per-filter the stash search applies. Names come from the panel, values are
                // bound, so the only thing interpolated is the index.
                var filters = statFilters?.Where(f => f is { Length: > 0 }).ToList() ?? new List<string[]>();
                var filterClause = string.Concat(filters.Select((_, i) => $@"
                    AND EXISTS (SELECT 1 FROM DatabaseItemStat_v2 F{i}
                          WHERE F{i}.id_databaseitem = I.id_databaseitem AND F{i}.Stat IN (:statfilter{i}))"));

                // One pass with correlated subqueries rather than three joins: DatabaseItemStat_v2 has ~1.8M
                // rows, and its (id_databaseitem, Stat) index makes each lookup a point read.
                var query = session.CreateSQLQuery(@"
                    SELECT I.baserecord AS BaseRecord,
                           I.name AS Name,
                           (SELECT S.TextValue FROM DatabaseItemStat_v2 S
                             WHERE S.id_databaseitem = I.id_databaseitem AND S.Stat = 'itemClassification') AS Rarity,
                           (SELECT S.TextValue FROM DatabaseItemStat_v2 S
                             WHERE S.id_databaseitem = I.id_databaseitem AND S.Stat = 'bitmap') AS Icon,
                           (SELECT S.TextValue FROM DatabaseItemStat_v2 S
                             WHERE S.id_databaseitem = I.id_databaseitem AND S.Stat = 'Class') AS ItemClass,
                           COALESCE((SELECT S.val1 FROM DatabaseItemStat_v2 S
                             WHERE S.id_databaseitem = I.id_databaseitem AND S.Stat = 'levelRequirement'), 0) AS Level
                    FROM DatabaseItem_v2 I
                    WHERE I.name IS NOT NULL AND I.name != ''
                    AND I.baserecord LIKE 'records/items/%'
                    -- Filter on Class, not on itemClassification: quest notes ARE classified (mostly as
                    -- Common), so keying off the classification kept every one of them. Class is what
                    -- separates equipment from lore notes, blueprints and props.
                    AND NOT EXISTS (SELECT 1 FROM DatabaseItemStat_v2 C
                          WHERE C.id_databaseitem = I.id_databaseitem AND C.Stat = 'Class'
                          AND C.TextValue IN ('ItemNote', 'ItemDifficultyUnlock', 'ItemUsableSkill', 'QuestItem'))
                    AND (:fragment = '' OR I.namelowercase LIKE :like)
                    AND (:rarity = '' OR EXISTS (SELECT 1 FROM DatabaseItemStat_v2 R
                          WHERE R.id_databaseitem = I.id_databaseitem
                          AND R.Stat = 'itemClassification' AND R.TextValue = :rarity))
                    AND (:maxLevel = 0 OR COALESCE((SELECT S.val1 FROM DatabaseItemStat_v2 S
                          WHERE S.id_databaseitem = I.id_databaseitem AND S.Stat = 'levelRequirement'), 0)
                          BETWEEN :minLevel AND :maxLevel)"
                    + slotClause + filterClause + @"
                    ORDER BY I.name, Level
                    LIMIT :limit")
                    .SetParameter("fragment", nameFragment ?? string.Empty)
                    .SetParameter("like", "%" + (nameFragment ?? string.Empty).ToLowerInvariant() + "%")
                    .SetParameter("rarity", RarityToClassification(rarityColour))
                    .SetParameter("minLevel", minLevel)
                    .SetParameter("maxLevel", maxLevel)
                    .SetParameter("limit", limit)
                    .SetResultTransformer(Transformers.AliasToBean<DatabaseBrowseRow>());

                if (hasSlots) {
                    query.SetParameterList("slots", slotClasses);
                }

                for (var i = 0; i < filters.Count; i++) {
                    query.SetParameterList($"statfilter{i}", filters[i]);
                }

                var rows = query.List<DatabaseBrowseRow>();

                foreach (var row in rows) {
                    row.Icon = IconFileName(row.Icon);
                }

                return rows;
            }
            catch (Exception ex) {
                Logger.Warn($"Database browse failed for \"{nameFragment}\": {ex.Message}");
                return new List<DatabaseBrowseRow>();
            }
        }

        private static string RarityToClassification(string? colour) {
            if (string.IsNullOrEmpty(colour)) {
                return string.Empty; // "Any"
            }

            if (ClassificationByColour.TryGetValue(colour!, out var classification)) {
                return classification;
            }

            // Falling back to "" would mean "no filter", which looks like the filter is broken rather than
            // unsupported. Pass the value through: it matches nothing, and the empty result says so.
            Logger.Warn($"No itemClassification known for rarity \"{colour}\"");
            return colour!;
        }

        /// <summary>
        /// "items/gearlegs/bitmaps/c004_legs.tex" -> "c004_legs.tex.png", which is how IAGD names the icons it
        /// extracts out of the game's .arc files into its storage folder.
        /// </summary>
        private static string? IconFileName(string? bitmapPath) {
            if (string.IsNullOrEmpty(bitmapPath)) {
                return null;
            }

            var file = bitmapPath!.Split('/', '\\').Last();
            return file + ".png";
        }
    }
}
