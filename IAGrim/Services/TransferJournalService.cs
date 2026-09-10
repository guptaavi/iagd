using EvilsoftCommons.Exceptions;
using IAGrim.Database;
using IAGrim.Database.Interfaces;
using IAGrim.Utilities;
using log4net;
using System.Collections.Concurrent;
using System.Text;

namespace IAGrim.Services {
    /// <summary>
    /// Applies the transfers the in-game item browser has already made.
    ///
    /// The injected hook creates the item in the player's stash itself and then leaves a note
    /// saying so. This is the other half: the note is read here and the item is taken out of
    /// the collection through the same path the client's own transfer uses, so an item handed
    /// over in-game is removed exactly as one handed over from the client is.
    ///
    /// Applying is idempotent, which is what makes an interrupted consume safe. Removal is
    /// keyed on the player item's id and the row is deleted; a note read a second time finds
    /// no such item and is discarded without touching anything. Nothing here decrements a
    /// stack, so nothing here can decrement one twice.
    ///
    /// The hook deliberately writes the note only after the game has confirmed the item was
    /// placed, so a note that exists means the player has the item.
    /// </summary>
    class TransferJournalService(IPlayerItemDao playerItemDao) : IDisposable {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(TransferJournalService));
        private readonly ConcurrentQueue<string> _queue = new();
        private volatile bool _isCancelled;
        private Thread? _thread;

        /// <summary>
        /// The cloud ids of items removed by a journal entry, so the client can tell the
        /// user's other machines that the item is gone before it can be transferred there too.
        /// The same signal the client raises for its own transfers.
        /// </summary>
        public event EventHandler<ItemsRemovedEventArgs>? OnItemsRemoved;

        public class ItemsRemovedEventArgs(List<string> cloudIds) : EventArgs {
            public List<string> CloudIds { get; } = cloudIds;
        }

        /// <summary>
        /// Applies everything already on disk, synchronously.
        ///
        /// Called before the item collection is published, so that an item transferred in a
        /// previous session is gone from the very first list the user sees rather than
        /// appearing and then vanishing -- and, more importantly, rather than being available
        /// to transfer a second time in the window between.
        /// </summary>
        public void ProcessOutstanding() {
            try {
                var files = Directory.GetFiles(GlobalPaths.TransferJournalLocation, "*.txt");
                if (files.Length == 0) {
                    return;
                }

                Logger.Info($"Applying {files.Length} outstanding in-game transfer(s) before publishing the collection");

                var removedCloudIds = new List<string>();
                foreach (var file in files) {
                    Apply(file, removedCloudIds);
                }

                if (removedCloudIds.Count > 0) {
                    OnItemsRemoved?.Invoke(this, new ItemsRemovedEventArgs(removedCloudIds));
                }
            }
            catch (Exception ex) {
                // A journal that cannot be read must not stop the client from starting: the
                // worst case is an item the user sees twice and can delete.
                Logger.Warn("Could not apply outstanding in-game transfers", ex);
            }
        }

        /// Watches for entries written while the client is running.
        public void Start() {
            _thread = new Thread(() => {
                ExceptionReporter.EnableLogUnhandledOnThread();

                while (!_isCancelled) {
                    Thread.Sleep(500);

                    if (!_queue.TryDequeue(out var file)) {
                        continue;
                    }

                    try {
                        var removedCloudIds = new List<string>();
                        Apply(file, removedCloudIds);

                        if (removedCloudIds.Count > 0) {
                            OnItemsRemoved?.Invoke(this, new ItemsRemovedEventArgs(removedCloudIds));
                        }
                    }
                    catch (IOException) {
                        // The hook publishes with a move, so a half-written file should not be
                        // visible -- but a virus scanner can still hold one for a moment.
                        _queue.Enqueue(file);
                    }
                    catch (Exception ex) {
                        Logger.Warn($"Could not apply in-game transfer {file}", ex);
                    }
                }
            }) {
                IsBackground = true
            };

            _thread.Start();
        }

        public void Queue(string filename) {
            _queue.Enqueue(filename);
        }

        public void Dispose() {
            _isCancelled = true;
            _thread = null;
        }

        /// <summary>
        /// Reads one entry, removes the item it names, and discards the entry.
        ///
        /// The entry is discarded last. A crash before that leaves it to be read again, which
        /// is harmless because the removal has already happened and the second read finds no
        /// item; a crash after the removal but before the discard is the same case.
        /// </summary>
        private void Apply(string filename, List<string> removedCloudIds) {
            var entry = Parse(filename);
            if (entry == null) {
                Discard(filename, "it could not be read");
                return;
            }

            var item = playerItemDao.GetById(entry.PlayerItemId);
            if (item == null) {
                // Already applied, or the user deleted it in the client first. Either way there
                // is nothing left to do and the note has served its purpose.
                Logger.Info($"In-game transfer {entry.Id}: item {entry.PlayerItemId} is already gone");
                Discard(filename, "the item is already gone");
                return;
            }

            var cloudId = item.CloudId;

            // The same two steps the client's own transfer takes: zero the stack, then let the
            // DAO delete the row, cascade the replica rows and mark the cloud id for deletion.
            item.StackCount = 0;
            playerItemDao.Update(new List<PlayerItem> { item }, true);

            if (!string.IsNullOrEmpty(cloudId)) {
                removedCloudIds.Add(cloudId!);
            }

            Logger.Info($"In-game transfer {entry.Id}: removed item {entry.PlayerItemId} ({entry.BaseRecord})");
            Discard(filename, "it has been applied");
        }

        private static void Discard(string filename, string why) {
            try {
                File.Delete(filename);
            }
            catch (Exception ex) {
                // Left behind, and read again next time. Apply is idempotent, so that costs a
                // log line rather than an item.
                Logger.Warn($"Could not delete the in-game transfer note {filename} after {why}", ex);
            }
        }

        private class JournalEntry {
            public string Id { get; init; } = string.Empty;
            public long PlayerItemId { get; init; }
            public string BaseRecord { get; init; } = string.Empty;
        }

        /// <summary>
        /// Reads the hook's "key=value" note. Unknown keys are ignored so the hook can add a
        /// field without this having to be updated in the same release.
        /// </summary>
        private static JournalEntry? Parse(string filename) {
            var values = new Dictionary<string, string>();

            foreach (var line in File.ReadAllLines(filename, Encoding.UTF8)) {
                var separator = line.IndexOf('=');
                if (separator <= 0) {
                    continue;
                }

                values[line[..separator].Trim().ToLowerInvariant()] = line[(separator + 1)..].Trim();
            }

            if (!values.TryGetValue("playeritemid", out var rawId) || !long.TryParse(rawId, out var playerItemId)) {
                Logger.Warn($"The in-game transfer note {filename} has no usable player item id");
                return null;
            }

            return new JournalEntry {
                Id = values.GetValueOrDefault("id", string.Empty),
                PlayerItemId = playerItemId,
                BaseRecord = values.GetValueOrDefault("baserecord", string.Empty)
            };
        }
    }
}
