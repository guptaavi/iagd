using IAGrim.Database;
using IAGrim.Database.Interfaces;
using IAGrim.Parsers.Arz;
using IAGrim.UI.Misc.CEF;
using IAGrim.Utilities;
using log4net;
using System;
using System.Collections.Generic;
using System.Linq;
using IAGrim.UI.Misc;
using IAGrim.Settings;
using System.Windows.Forms;
using static IAGrim.UI.StashPicker;

namespace IAGrim.UI.Controller {
    internal class ItemTransferController {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(ItemTransferController));
        private readonly IPlayerItemDao _dao;
        private readonly Action<string> _setFeedback;
        private readonly CefBrowserHandler _browser;
        private readonly TransferStashService _transferStashService;
        private readonly SettingsService _settingsService;
        private readonly System.Threading.Timer _bagAckTimer;
        private int _processingBagAcks;

        /// <summary>
        /// Raised after items have been transferred back in-game and removed locally, carrying the cloud ids of the removed items so the deletion can be synced live.
        /// </summary>
        public event EventHandler<ItemsTransferredEventArgs>? OnItemsTransferredToGame;

        public class ItemsTransferredEventArgs : EventArgs {
            public IList<string> CloudIds { get; }
            public IList<long> PlayerItemIds { get; }

            public ItemsTransferredEventArgs(IList<string> cloudIds, IList<long>? playerItemIds = null) {
                CloudIds = cloudIds;
                PlayerItemIds = playerItemIds ?? new List<long>();
            }
        }

        public ItemTransferController(
            CefBrowserHandler browser,
            Action<string> feedback,
            IPlayerItemDao playerItemDao,
            TransferStashService transferStashService,
            SettingsService settingsService
            ) {
            _browser = browser;
            _setFeedback = feedback;
            _dao = playerItemDao;
            _transferStashService = transferStashService;
            _settingsService = settingsService;

            // Bag transfers are acknowledged by the native hook on the game's update thread. Keep the DB
            // item until that acknowledgement arrives; this also makes a full bag recoverable rather than
            // silently deleting the item from IA.
            _bagAckTimer = new System.Threading.Timer(_ => ProcessBagAcknowledgements(), null, 1000, 1000);
        }


        List<PlayerItem>? GetItemsForTransfer(StashTransferEventArgs args) {
            List<PlayerItem> items = new List<PlayerItem>();

            // Detect the record type (long or string) and add the item(s)
            if (args.HasValidId) {
                var pid = args.PlayerItemId;
                if (pid.HasValue) {
                    var item = _dao.GetById(pid.Value);
                    items.Add(item);
                }
                else {
                    // The frontend merges items into a single stack on base/prefix/suffix only, ignoring the
                    // component (materia). Match on the same key when transferring all of them, otherwise every
                    // item in the stack that has a component is silently left behind.
                    var materia = args.TransferAll ? null : args.Materia;

                    // HasValidId (checked above) guarantees these are non-null.
                    IList<PlayerItem> tmp = _dao.GetByRecord(args.Prefix!, args.BaseRecord!, args.Suffix!, materia, args.Mod!, args.IsHardcore);
                    if (tmp.Count > 0) {
                        if (!args.TransferAll)
                            Logger.Warn("Error transferring item, transfer all was false, but no player item id was located.");
                        else {
                            items.AddRange(tmp);
                        }
                    }
                }
            }

            if (items.Any(i => i == null)) {
                Logger.Warn("Attempted to transfer NULL item.");

                var message = RuntimeSettings.Language!.GetTag("iatag_feedback_item_does_not_exist");
                _setFeedback(message);
                _browser.ShowMessage(message, UserFeedbackLevel.Danger);

                return null;
            }

            return items;
        }

        struct TransferStatus {
            public int NumItemsTransferred;
        }


        private TransferStatus TransferItems(List<PlayerItem> items, StashPickerResult? modOverride) {
            int numItemsReceived = (int)items.Sum(item => Math.Max(1, item.StackCount));
            _transferStashService.Deposit(items, modOverride);
            _dao.Update(items, true);

            var markedForDeletion = new HashSet<string>(_dao.GetItemsMarkedForOnlineDeletion().Select(d => d.Id!));
            var removedCloudIds = items
                .Where(item => !string.IsNullOrEmpty(item.CloudId) && markedForDeletion.Contains(item.CloudId))
                .Select(item => item.CloudId!)
                .ToList();
            if (removedCloudIds.Count > 0) {
                OnItemsTransferredToGame?.Invoke(this, new ItemsTransferredEventArgs(removedCloudIds));
            }

            return new TransferStatus {
                NumItemsTransferred = (int)numItemsReceived,
            };
        }


        /// <summary>
        /// Transfer item request from sub control
        /// MUST BE CALLED ON SQL THREAD
        /// </summary>
        public void TransferItem(StashTransferEventArgs args) {
            Logger.Debug($"Item transfer requested, arguments: {args}");

            List<PlayerItem>? items = GetItemsForTransfer(args);

            StashPickerResult? modOverride = null;
            if (items?.Count > 0) {
                if (_settingsService.GetPersistent().TransferAnyMod) {
                    StashPicker picker = new StashPicker(_browser, _dao, _settingsService);
                    if (picker.ShowDialog() == DialogResult.OK) {
                        modOverride = picker.Result;
                    }
                }

                var result = TransferItems(items, modOverride);
                args.NumTransferred = result.NumItemsTransferred;
                args.IsSuccessful = true;
                var message = RuntimeSettings.Language!.GetTag("iatag_stash3_success", result.NumItemsTransferred);
                _browser.ShowMessage(message, UserFeedbackLevel.Success);
            }
            else {
                Logger.Warn("Could not find any items for the requested transfer");
                _browser.ShowMessage(RuntimeSettings.Language!.GetTag("iatag_feedback_unable_to_deposit"), UserFeedbackLevel.Warning);

            }
        }

        /// <summary>Queue a bounded batch for the player's bags; deletion waits for native success.</summary>
        public void TransferItemsToBags(BagTransferEventArgs args) {
            var items = args.PlayerItemIds
                .Take(args.BatchSize)
                .Distinct()
                .Select(id => _dao.GetById(id))
                .Where(item => item != null && item.StackCount > 0)
                .ToList();

            if (items.Count == 0) {
                _browser.ShowMessage("No owned items were available for bag transfer.", UserFeedbackLevel.Warning);
                return;
            }

            args.NumQueued = _transferStashService.DepositToPlayerBags(items);
            args.IsSuccessful = args.NumQueued > 0;
            if (args.IsSuccessful) {
                var skipped = items.Count - args.NumQueued;
                var suffix = skipped > 0 ? $" Skipped {skipped} already-pending item(s)." : string.Empty;
                _browser.ShowMessage($"Queued {args.NumQueued} item stack(s) for the player bags.{suffix} Full bags remain recoverable.", UserFeedbackLevel.Success);
            }
            else {
                _browser.ShowMessage("All selected items are already queued for the player bags.", UserFeedbackLevel.Warning);
            }
        }

        private void ProcessBagAcknowledgements() {
            if (Interlocked.Exchange(ref _processingBagAcks, 1) != 0) {
                return;
            }

            try {
                var transferredCloudIds = new List<string>();
                var transferredPlayerItemIds = new List<long>();

                foreach (var file in Directory.EnumerateFiles(GlobalPaths.CsvLocationCompletedBags, "*.csv", SearchOption.AllDirectories)) {
                    var name = Path.GetFileNameWithoutExtension(file);
                    var separator = name.IndexOf('_');
                    if (separator <= 0 || !long.TryParse(name[..separator], out var id)) {
                        Logger.Warn($"Ignoring completed bag acknowledgement with invalid name: {file}");
                        continue;
                    }

                    var item = _dao.GetById(id);
                    if (item != null) {
                        var cloudId = item.CloudId;
                        item.StackCount = 0;
                        _dao.Update(new List<PlayerItem> { item }, true);
                        JunkItemStore.Remove(id);
                        if (!string.IsNullOrEmpty(cloudId)) {
                            transferredCloudIds.Add(cloudId);
                        }
                        transferredPlayerItemIds.Add(id);
                        Logger.Info($"Native hook acknowledged player-bag transfer for item {id}");
                    }

                    File.Delete(file);
                }

                // Raise one batched notification after all acknowledgements have been applied. This lets
                // the native grid remove the transferred rows in one UI update instead of refreshing once
                // per item, and keeps the visible Junk state in sync with the database.
                if (transferredPlayerItemIds.Count > 0 || transferredCloudIds.Count > 0) {
                    OnItemsTransferredToGame?.Invoke(this, new ItemsTransferredEventArgs(transferredCloudIds, transferredPlayerItemIds));
                }
            }
            catch (Exception ex) {
                Logger.Warn($"Could not process player-bag acknowledgements: {ex.Message}");
            }
            finally {
                Volatile.Write(ref _processingBagAcks, 0);
            }
        }
    }
}
