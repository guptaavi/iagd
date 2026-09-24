using IAGrim.Database.Model;
using log4net;
using NHibernate;
using NHibernate.Transform;

namespace IAGrim.Database.DAO {

    /// <summary>
    /// Item-set details for a single item, assembled from the game's own parsed records.
    ///
    /// Everything here comes out of DatabaseItem_v2/DatabaseItemStat_v2, which IAGD fills when it parses
    /// database.arz - the same source Grim Dawn itself reads, so no web lookup is involved:
    ///
    ///   item record --itemSetName--> set record --setName/setDescription--> ItemTag (display strings)
    ///   set record  &lt;--itemSetName-- every other piece of the set (the reverse edge gives the member list)
    ///
    /// NOT available from this data: drop sources ("Obtained from"). Loot tables are a separate part of the
    /// game database that IAGD does not parse, which is why that section has to come from elsewhere.
    /// </summary>
    public class ItemSetDetails {
        public string SetName { get; set; } = string.Empty;
        public string? Description { get; set; }

        /// <summary>Every piece of the set, in record order, with the display name shown in game.</summary>
        public List<ItemSetMember> Members { get; set; } = new();

        /// <summary>Set bonuses as the set record stores them, flattened (the tier they belong to is not kept).</summary>
        public List<string> Bonuses { get; set; } = new();
    }

    public class ItemSetMember {
        public string BaseRecord { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;

        /// <summary>
        /// 1 when this exact record is already in the user's own item database, 0 otherwise.
        ///
        /// Typed as long rather than bool on purpose: SQLite has no boolean, so EXISTS() comes back as an
        /// INTEGER and NHibernate's AliasToBean transformer throws "Object of type 'System.Int64' cannot be
        /// converted to type 'System.Boolean'" when the property is a bool.
        /// </summary>
        public long OwnedFlag { get; set; }

        public bool Owned => OwnedFlag != 0;
    }

    public class SetLookup {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(SetLookup));
        private readonly SessionFactory _sessionCreator;

        /// <summary>Records with no set resolve to null and are cached as such; most items are not set items.</summary>
        private readonly Dictionary<string, ItemSetDetails?> _cache = new();

        public SetLookup(SessionFactory sessionCreator) {
            _sessionCreator = sessionCreator;
        }

        public ItemSetDetails? Find(string? baseRecord) {
            if (string.IsNullOrEmpty(baseRecord)) {
                return null;
            }

            lock (_cache) {
                if (_cache.TryGetValue(baseRecord, out var cached)) {
                    return cached;
                }
            }

            ItemSetDetails? details = null;
            try {
                details = Query(baseRecord!);
            }
            catch (Exception ex) {
                Logger.Warn($"Could not look up the item set for {baseRecord}: {ex.Message}");
            }

            lock (_cache) {
                _cache[baseRecord!] = details;
            }

            return details;
        }

        private ItemSetDetails? Query(string baseRecord) {
            using ISession session = _sessionCreator.OpenSession();

            var setRecord = session.CreateSQLQuery(@"
                SELECT S.TextValue
                FROM DatabaseItemStat_v2 S, DatabaseItem_v2 I
                WHERE S.id_databaseitem = I.id_databaseitem
                AND I.baserecord = :record
                AND S.Stat = 'itemSetName'")
                .SetParameter("record", baseRecord)
                .UniqueResult<string>();

            if (string.IsNullOrEmpty(setRecord)) {
                return null;
            }

            // setName/setDescription hold tags, not text; ItemTag turns them into what the game displays.
            var labels = session.CreateSQLQuery(@"
                SELECT S.Stat AS Stat, COALESCE(T.Name, S.TextValue) AS Label
                FROM DatabaseItemStat_v2 S
                JOIN DatabaseItem_v2 I ON I.id_databaseitem = S.id_databaseitem
                LEFT JOIN ItemTag T ON T.Tag = S.TextValue
                WHERE I.baserecord = :record
                AND S.Stat IN ('setName', 'setDescription')")
                .SetParameter("record", setRecord)
                .SetResultTransformer(Transformers.AliasToBean<StatLabel>())
                .List<StatLabel>();

            var name = labels.FirstOrDefault(l => l.Stat == "setName")?.Label;
            if (string.IsNullOrEmpty(name)) {
                return null;
            }

            var members = session.CreateSQLQuery(@"
                SELECT I.baserecord AS BaseRecord, I.name AS Name,
                       EXISTS (SELECT 1 FROM PlayerItem P WHERE P.baserecord = I.baserecord) AS OwnedFlag
                FROM DatabaseItemStat_v2 S
                JOIN DatabaseItem_v2 I ON I.id_databaseitem = S.id_databaseitem
                WHERE S.Stat = 'itemSetName'
                AND S.TextValue = :setRecord
                AND I.name IS NOT NULL
                ORDER BY I.name")
                .SetParameter("setRecord", setRecord)
                .SetResultTransformer(Transformers.AliasToBean<ItemSetMember>())
                .List<ItemSetMember>();

            return new ItemSetDetails {
                SetName = name!,
                Description = labels.FirstOrDefault(l => l.Stat == "setDescription")?.Label,
                Members = members.ToList(),
                Bonuses = new List<string>(),
            };
        }

        /// <summary>Row shape for the label query; NHibernate's bean transformer needs a settable type.</summary>
        public class StatLabel {
            public string Stat { get; set; } = string.Empty;
            public string? Label { get; set; }
        }
    }
}
