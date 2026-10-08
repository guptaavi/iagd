using IAGrim.Database;
using log4net;
using System.Globalization;

namespace IAGrim.Utilities {
    /// <summary>
    /// Persistent IA-side junk marks. The database remains the source of item data; this small sidecar only
    /// records exact PlayerItem ids so marking never relies on a display name or an item stack's merge key.
    /// </summary>
    internal static class JunkItemStore {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(JunkItemStore));
        private static readonly object Sync = new();
        private static HashSet<long>? _ids;

        private static string Path => System.IO.Path.Combine(GlobalPaths.UserdataFolder, "junk-items.txt");

        private static HashSet<long> Load() {
            var result = new HashSet<long>();
            try {
                if (File.Exists(Path)) {
                    foreach (var line in File.ReadAllLines(Path)) {
                        if (long.TryParse(line.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out var id)) {
                            result.Add(id);
                        }
                    }
                }
            }
            catch (Exception ex) {
                Logger.Warn($"Could not load Junk marks: {ex.Message}");
            }

            return result;
        }

        private static HashSet<long> Ids => _ids ??= Load();

        public static bool Contains(long id) {
            lock (Sync) {
                return Ids.Contains(id);
            }
        }

        public static void Add(IEnumerable<long> ids) {
            lock (Sync) {
                foreach (var id in ids) {
                    if (id > 0) Ids.Add(id);
                }
                Save();
            }
        }

        public static void Remove(IEnumerable<long> ids) {
            lock (Sync) {
                foreach (var id in ids) Ids.Remove(id);
                Save();
            }
        }

        public static void Remove(long id) => Remove(new[] { id });

        private static void Save() {
            try {
                Directory.CreateDirectory(GlobalPaths.UserdataFolder);
                var temp = Path + ".tmp";
                File.WriteAllLines(temp, Ids.OrderBy(id => id).Select(id => id.ToString(CultureInfo.InvariantCulture)));
                File.Move(temp, Path, true);
            }
            catch (Exception ex) {
                Logger.Warn($"Could not save Junk marks: {ex.Message}");
            }
        }
    }
}
